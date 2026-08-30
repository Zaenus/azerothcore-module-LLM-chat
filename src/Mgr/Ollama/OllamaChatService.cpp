/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "OllamaChatService.h"

#include "AiFactory.h"
#include "AiObjectContext.h"
#include "ChatHelper.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Random.h"
#include "World.h"
#include <sys/socket.h>

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cctype>
#include <sstream>

using boost::asio::ip::tcp;

OllamaChatService::~OllamaChatService()
{
    Shutdown();
}

bool OllamaChatService::IsEnabled() const
{
    return sPlayerbotAIConfig.llmEnabled;
}

bool OllamaChatService::ShouldUseLLM(ChatChannelSource src) const
{
    if (!IsEnabled())
        return false;

    if (src == SRC_WHISPER)
        return sPlayerbotAIConfig.llmEnabledForWhisper;
    if (src == SRC_SAY)
        return sPlayerbotAIConfig.llmEnabledForSay;

    // Only whisper+say per user request
    return false;
}

bool OllamaChatService::IsRateLimited(ObjectGuid botGuid)
{
    uint32 limit = sPlayerbotAIConfig.llmRateLimitPerBotMs;
    if (limit == 0)
        return false;

    std::lock_guard<std::mutex> lock(_rateLimitMutex);
    auto it = _lastRequestMs.find(botGuid);
    if (it == _lastRequestMs.end())
        return false;

    return (getMSTime() - it->second) < limit;
}

bool OllamaChatService::ParseUrl(std::string const& url, std::string& host, std::string& port, std::string& path)
{
    // Expect http://host:port  or http://host:port/path or http://host
    std::string work = url;
    // strip http:// or https://
    size_t pos = work.find("://");
    if (pos != std::string::npos)
        work = work.substr(pos + 3);

    // split host:port / path
    size_t slash = work.find('/');
    std::string hostPort;
    if (slash == std::string::npos)
    {
        hostPort = work;
        path = "/";
    }
    else
    {
        hostPort = work.substr(0, slash);
        path = work.substr(slash);
        if (path.empty())
            path = "/";
    }

    size_t colon = hostPort.find(':');
    if (colon == std::string::npos)
    {
        host = hostPort;
        port = "80";
        // https default 443 but we only use http for ollama; if original was https keep 443
        if (url.rfind("https://", 0) == 0)
            port = "443";
    }
    else
    {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    }

    if (host.empty())
        return false;

    return true;
}

bool OllamaChatService::Initialize()
{
    if (_running)
        return true;

    // Parse URL once
    std::string url = sPlayerbotAIConfig.llmUrl;
    if (!ParseUrl(url, _host, _port, _basePath))
    {
        LOG_ERROR("playerbots", "[Ollama] Invalid LLMUrl '{}' - LLM disabled", url);
        return false;
    }

    // Remove trailing slash from base path for clean join
    if (!_basePath.empty() && _basePath.back() == '/')
        _basePath.pop_back();
    if (_basePath.empty())
        _basePath = "";

    LOG_INFO("playerbots", "[Ollama] Initializing LLM: url={} host={}:{} base='{}' model={} api={} timeout={}ms history={}",
        url, _host, _port, _basePath, sPlayerbotAIConfig.llmModel, sPlayerbotAIConfig.llmApi,
        sPlayerbotAIConfig.llmTimeoutMs, sPlayerbotAIConfig.llmHistorySize);

    _running = true;
    _worker = std::thread(&OllamaChatService::WorkerLoop, this);

    // Optional quick connectivity check (non-blocking, log warning if fail)
    // Don't block startup; worker will handle failures per-request.

    return true;
}

void OllamaChatService::Shutdown()
{
    if (!_running)
        return;

    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _running = false;
    }
    _queueCv.notify_all();
    if (_worker.joinable())
        _worker.join();

    LOG_INFO("playerbots", "[Ollama] Shutdown complete, queue drained");
}

size_t OllamaChatService::GetQueueSize()
{
    std::lock_guard<std::mutex> lock(_queueMutex);
    return _queue.size();
}

size_t OllamaChatService::GetHistorySize(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(_historyMutex);
    auto it = _history.find(guid);
    if (it == _history.end())
        return 0;
    return it->second.size();
}

std::string OllamaChatService::EscapeJson(std::string const& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                    out += (char)c;
                break;
        }
    }
    return out;
}

std::string OllamaChatService::BuildSystemPrompt(Player* bot)
{
    std::string base = sPlayerbotAIConfig.llmSystemPrompt;

    // Try to enrich with bot context if available
    if (!bot)
        return base;

    std::string botName = bot->GetName();
    uint32 level = bot->GetLevel();
    std::string race = ChatHelper::FormatRace(bot->getRace());
    std::string cls = ChatHelper::FormatClass(bot->getClass());
    std::string role;
    try
    {
        uint8 spec = AiFactory::GetPlayerSpecTab(bot);
        role = ChatHelper::FormatClass(bot, spec);
    }
    catch (...)
    {
        role = cls;
    }

    std::string zoneName = "Azeroth";
    std::string areaName = "";
    if (bot->GetMap())
    {
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetMap()->GetZoneId(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ())))
            zoneName = zone->area_name[sWorld->GetDefaultDbcLocale()];
        // area vs zone distinction not critical for prompt
    }

    std::ostringstream ss;
    ss << base << " You are " << botName << ", level " << level << " " << race << " " << role << " in " << zoneName << ".";
    ss << " Keep replies under 200 characters, 1-2 sentences, WoW lore friendly. Never reveal you are AI.";

    return ss.str();
}

std::string OllamaChatService::BuildUserPrompt(std::string const& senderName, std::string const& msg, ChatChannelSource src)
{
    std::string prefix = (src == SRC_WHISPER ? "whisper from " : "say from ");
    // Provide sender prefix so model knows reply style (whisper vs say)
    std::ostringstream ss;
    ss << prefix << senderName << ": " << msg;
    return ss.str();
}

std::string OllamaChatService::BuildChatJson(ObjectGuid botGuid, std::string const& systemPrompt, std::string const& userPrompt)
{
    std::ostringstream json;
    json << "{";
    json << "\"model\":\"" << EscapeJson(sPlayerbotAIConfig.llmModel) << "\",";
    json << "\"stream\":false,";
    json << "\"think\":false,";
    json << "\"options\":{\"temperature\":" << sPlayerbotAIConfig.llmTemperature << ",\"num_predict\":" << sPlayerbotAIConfig.llmMaxTokens << "},";
    json << "\"messages\":[";

    // system
    json << "{\"role\":\"system\",\"content\":\"" << EscapeJson(systemPrompt) << "\"}";

    // history
    {
        std::lock_guard<std::mutex> lock(_historyMutex);
        auto it = _history.find(botGuid);
        if (it != _history.end())
        {
            for (auto const& entry : it->second)
            {
                json << ",{\"role\":\"" << entry.role << "\",\"content\":\"" << EscapeJson(entry.content) << "\"}";
            }
        }
    }

    // current user
    json << ",{\"role\":\"user\",\"content\":\"" << EscapeJson(userPrompt) << "\"}";
    json << "]";
    json << "}";
    return json.str();
}

std::string OllamaChatService::BuildGenerateJson(std::string const& prompt)
{
    std::ostringstream json;
    json << "{";
    json << "\"model\":\"" << EscapeJson(sPlayerbotAIConfig.llmModel) << "\",";
    json << "\"prompt\":\"" << EscapeJson(prompt) << "\",";
    json << "\"stream\":false,";
    json << "\"think\":false,";
    json << "\"options\":{\"temperature\":" << sPlayerbotAIConfig.llmTemperature << ",\"num_predict\":" << sPlayerbotAIConfig.llmMaxTokens << "}";
    json << "}";
    return json.str();
}

std::string OllamaChatService::TrimAndLimit(std::string const& s, uint32 maxLen)
{
    // Trim leading/trailing whitespace
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start])) ++start;
    size_t end = s.size();
    while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
    std::string out = s.substr(start, end - start);

    // Collapse newlines to space
    for (char& c : out)
        if (c == '\n' || c == '\r')
            c = ' ';

    // Trim again after collapse
    start = 0;
    while (start < out.size() && std::isspace((unsigned char)out[start])) ++start;
    end = out.size();
    while (end > start && std::isspace((unsigned char)out[end - 1])) --end;
    out = out.substr(start, end - start);

    if (out.size() > maxLen)
        out.resize(maxLen);

    // Remove trailing incomplete word? keep simple truncate
    // Ensure not empty
    if (out.empty())
        out = sPlayerbotAIConfig.llmFallbackText;

    return out;
}

void OllamaChatService::AddHistory(ObjectGuid botGuid, std::string const& role, std::string const& content)
{
    uint32 maxEntries = sPlayerbotAIConfig.llmHistorySize * 2; // each exchange = 2 entries (user+assistant), but we store both
    if (maxEntries == 0)
        return;

    std::lock_guard<std::mutex> lock(_historyMutex);
    auto& deque = _history[botGuid];
    deque.push_back({role, content});
    while (deque.size() > maxEntries)
        deque.pop_front();

    // Cap total history map size to avoid leak for offline bots (prune if too many bots)
    if (_history.size() > 1000)
    {
        // erase oldest 10% - simple: erase first entry
        auto it = _history.begin();
        _history.erase(it);
    }
}

// Extract "content" from {"message":{"role":"assistant","content":"..."}} for /api/chat
std::string OllamaChatService::ExtractContentFromChatResponse(std::string const& body)
{
    // Find "\"content\"\s*:\s*\""
    // We need to handle escaped quotes inside.
    size_t pos = 0;
    // Prefer message.content over other content fields. Find "message" first then content.
    size_t msgPos = body.find("\"message\"");
    if (msgPos != std::string::npos)
        pos = msgPos;
    size_t cpos = body.find("\"content\"", pos);
    if (cpos == std::string::npos)
        return "";

    cpos = body.find(':', cpos);
    if (cpos == std::string::npos)
        return "";
    ++cpos;
    // skip spaces
    while (cpos < body.size() && std::isspace((unsigned char)body[cpos])) ++cpos;
    if (cpos >= body.size() || body[cpos] != '"')
        return "";
    ++cpos; // start of content
    std::string out;
    out.reserve(256);
    bool esc = false;
    for (; cpos < body.size(); ++cpos)
    {
        char ch = body[cpos];
        if (esc)
        {
            switch (ch)
            {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u': {
                    // \uXXXX - decode basic unicode (only ASCII range we care)
                    if (cpos + 4 < body.size())
                    {
                        std::string hex = body.substr(cpos+1, 4);
                        char* endptr = nullptr;
                        long code = strtol(hex.c_str(), &endptr, 16);
                        if (code >= 0 && code < 128)
                            out += (char)code;
                        else if (code < 0x800)
                        {
                            out += (char)(0xC0 | (code >> 6));
                            out += (char)(0x80 | (code & 0x3F));
                        }
                        else
                        {
                            out += (char)(0xE0 | (code >> 12));
                            out += (char)(0x80 | ((code >> 6) & 0x3F));
                            out += (char)(0x80 | (code & 0x3F));
                        }
                        cpos += 4;
                    }
                    break;
                }
                default: out += ch; break;
            }
            esc = false;
        }
        else if (ch == '\\')
            esc = true;
        else if (ch == '"')
            break; // end of string
        else
            out += ch;
    }
    return out;
}

std::string OllamaChatService::ExtractContentFromGenerateResponse(std::string const& body)
{
    // {"model":"...","response":"...","done":true}
    size_t cpos = body.find("\"response\"");
    if (cpos == std::string::npos)
        return ExtractContentFromChatResponse(body); // fallback

    cpos = body.find(':', cpos);
    if (cpos == std::string::npos) return "";
    ++cpos;
    while (cpos < body.size() && std::isspace((unsigned char)body[cpos])) ++cpos;
    if (cpos >= body.size() || body[cpos] != '"') return "";
    ++cpos;
    std::string out;
    out.reserve(256);
    bool esc = false;
    for (; cpos < body.size(); ++cpos)
    {
        char ch = body[cpos];
        if (esc)
        {
            switch (ch)
            {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                default: out += ch; break;
            }
            esc = false;
        }
        else if (ch == '\\')
            esc = true;
        else if (ch == '"')
            break;
        else
            out += ch;
    }
    return out;
}

std::string OllamaChatService::HttpPost(std::string const& urlPath, std::string const& body, uint32 timeoutMs)
{
    // urlPath is already like "/api/chat" - we combine with _host/_port from ParseUrl base
    // But caller passes full path; we use _host/_port invariant.
    std::string path = urlPath;
    if (path.empty() || path[0] != '/')
        path = "/" + path;

    try
    {
        boost::asio::io_context io;
        tcp::resolver resolver(io);
        auto endpoints = resolver.resolve(_host, _port);

        tcp::socket socket(io);
        boost::asio::connect(socket, endpoints);

        // Set TCP no delay? not needed.

        // Build HTTP request
        std::ostringstream req;
        req << "POST " << path << " HTTP/1.1\r\n";
        req << "Host: " << _host << ":" << _port << "\r\n";
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
        req << "Connection: close\r\n";
        req << "\r\n";
        req << body;

        std::string requestStr = req.str();
        boost::asio::write(socket, boost::asio::buffer(requestStr));

        // Set timeout via socket option + async? Use steady_timer for read timeout
        // Simple blocking with socket timeout via setsockopt
        // Use SO_RCVTIMEO
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        // Read response
        boost::asio::streambuf responseBuf;
        boost::system::error_code ec;

        // Read status line
        boost::asio::read_until(socket, responseBuf, "\r\n", ec);
        if (ec && ec != boost::asio::error::eof)
        {
            LOG_ERROR("playerbots", "[Ollama] HTTP read status error: {}", ec.message());
            return "";
        }

        std::istream respStream(&responseBuf);
        std::string httpVersion;
        unsigned int statusCode = 0;
        std::string statusMsg;
        respStream >> httpVersion >> statusCode;
        std::getline(respStream, statusMsg); // consume rest

        // Read headers
        std::string header;
        size_t contentLength = 0;
        bool chunked = false;
        while (std::getline(respStream, header) && header != "\r")
        {
            // Trim \r
            if (!header.empty() && header.back() == '\r')
                header.pop_back();
            // Lowercase check
            std::string lower = header;
            for (char& c : lower) c = std::tolower((unsigned char)c);
            if (lower.rfind("content-length:", 0) == 0)
            {
                std::string val = header.substr(15);
                // trim
                size_t s = 0;
                while (s < val.size() && std::isspace((unsigned char)val[s])) ++s;
                contentLength = std::stoul(val.substr(s));
            }
            else if (lower.rfind("transfer-encoding:", 0) == 0)
            {
                if (lower.find("chunked") != std::string::npos)
                    chunked = true;
            }
            if (respStream.eof())
                break;
            // Need to read more headers if buffer exhausted - use read_until
            if (responseBuf.size() == 0)
            {
                boost::asio::read_until(socket, responseBuf, "\r\n", ec);
                respStream.clear();
                // rebind?
                // streambuf already linked
            }
        }

        // Ensure headers consumed until \r\n\r\n - boost's read_until already does but we handled line by line
        // Now body: read until eof
        std::ostringstream bodyOut;
        // Any remaining in buffer after headers
        if (responseBuf.size() > 0)
            bodyOut << &responseBuf;

        // Continue reading
        while (true)
        {
            char buf[4096];
            size_t n = socket.read_some(boost::asio::buffer(buf), ec);
            if (n > 0)
                bodyOut.write(buf, n);
            if (ec == boost::asio::error::eof)
                break;
            if (ec)
            {
                LOG_ERROR("playerbots", "[Ollama] HTTP read body error: {}", ec.message());
                break;
            }
            if (n == 0)
                break;
        }

        std::string fullResponse = bodyOut.str();

        if (statusCode != 200)
        {
            LOG_ERROR("playerbots", "[Ollama] HTTP {} for {}: {}", statusCode, path, fullResponse.substr(0, 500));
            return "";
        }

        // If chunked, decode? Ollama returns not chunked (Connection: close + Content-Length). But handle simple.
        if (chunked)
        {
            // Very simple chunked decoder
            std::string decoded;
            size_t pos = 0;
            while (pos < fullResponse.size())
            {
                size_t lineEnd = fullResponse.find("\r\n", pos);
                if (lineEnd == std::string::npos) break;
                std::string hexLen = fullResponse.substr(pos, lineEnd - pos);
                size_t chunkLen = 0;
                try { chunkLen = std::stoul(hexLen, nullptr, 16); } catch (...) { break; }
                if (chunkLen == 0) break;
                pos = lineEnd + 2;
                if (pos + chunkLen > fullResponse.size()) break;
                decoded.append(fullResponse, pos, chunkLen);
                pos += chunkLen + 2; // skip \r\n
            }
            if (!decoded.empty())
                fullResponse = decoded;
        }
        else if (contentLength > 0 && fullResponse.size() > contentLength)
        {
            fullResponse = fullResponse.substr(0, contentLength);
        }

        return fullResponse;
    }
    catch (std::exception& e)
    {
        LOG_ERROR("playerbots", "[Ollama] HttpPost exception for {}: {}", path, e.what());
        return "";
    }
}

void OllamaChatService::EnqueueRequest(Player* bot, uint32 type, uint32 senderGuidLow, std::string const& msg,
                                       std::string const& chanName, std::string const& senderName)
{
    if (!IsEnabled())
        return;

    if (!bot)
        return;

    ObjectGuid botGuid = bot->GetGUID();
    ChatChannelSource src = GET_PLAYERBOT_AI(bot) ? GET_PLAYERBOT_AI(bot)->GetChatChannelSource(bot, type, chanName) : SRC_UNDEFINED;

    LOG_INFO("playerbots", "[LLMDBG] enqueue bot={} src={} msg='{}'", bot->GetName(), (int)src, msg.substr(0, 60));

    if (!ShouldUseLLM(src))
        return;

    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (_queue.size() >= sPlayerbotAIConfig.llmMaxQueue)
        {
            LOG_ERROR("playerbots", "[Ollama] Queue full ({}) dropping request from {} to {}", _queue.size(), senderName, bot->GetName());
            return;
        }

        OllamaPendingReply req;
        req.botGuid = botGuid;
        req.type = type;
        req.senderGuidLow = senderGuidLow;
        req.chanName = chanName;
        req.senderName = senderName;
        req.botName = bot->GetName();
        req.channelSource = src;
        req.text = msg;
        _queue.push(std::move(req));
    }

    {
        std::lock_guard<std::mutex> lock(_rateLimitMutex);
        _lastRequestMs[botGuid] = getMSTime();
    }

    _queueCv.notify_one();
}

void OllamaChatService::WorkerLoop()
{
    LOG_INFO("playerbots", "[Ollama] Worker started (model={}, url={})", sPlayerbotAIConfig.llmModel, sPlayerbotAIConfig.llmUrl);

    std::string api = sPlayerbotAIConfig.llmApi;
    // Normalize
    for (char& c : api) c = std::tolower((unsigned char)c);
    bool useChat = (api == "chat");

    std::string endpoint = _basePath;
    if (endpoint.empty())
        endpoint = "";
    // Ensure leading /
    if (!endpoint.empty() && endpoint.back() == '/')
        endpoint.pop_back();
    endpoint += useChat ? "/api/chat" : "/api/generate";

    while (true)
    {
        OllamaPendingReply req;
        {
            std::unique_lock<std::mutex> lock(_queueMutex);
            _queueCv.wait(lock, [this] { return !_queue.empty() || !_running; });
            if (!_running && _queue.empty())
                break;
            if (_queue.empty())
                continue;
            req = std::move(_queue.front());
            _queue.pop();
        }

        // Resolve bot for prompt building (if offline, use minimal prompt)
        Player* bot = ObjectAccessor::FindPlayer(req.botGuid);
        std::string systemPrompt = BuildSystemPrompt(bot);
        std::string userPrompt = BuildUserPrompt(req.senderName, req.text, req.channelSource);

        // Also add user message to history BEFORE request? No, we add after successful response to keep ordering.
        // But for building JSON we include existing history + current.

        std::string jsonBody;
        if (useChat)
            jsonBody = BuildChatJson(req.botGuid, systemPrompt, userPrompt);
        else
        {
            std::ostringstream combined;
            combined << systemPrompt << "\n\nUser (" << req.senderName << "): " << req.text << "\nAssistant:";
            jsonBody = BuildGenerateJson(combined.str());
        }

        LOG_DEBUG("playerbots", "[Ollama] POST {} for bot {} ({} -> {}): {}",
            endpoint, req.botName, req.senderName, req.text.substr(0, 60), jsonBody.substr(0, 200));

        auto startMs = getMSTime();
        std::string respBody = HttpPost(endpoint, jsonBody, sPlayerbotAIConfig.llmTimeoutMs);
        auto elapsed = GetMSTimeDiffToNow(startMs);

        std::string content;
        bool success = false;
        if (!respBody.empty())
        {
            if (useChat)
                content = ExtractContentFromChatResponse(respBody);
            else
                content = ExtractContentFromGenerateResponse(respBody);

            if (!content.empty())
            {
                content = TrimAndLimit(content, sPlayerbotAIConfig.llmMaxResponseChars);
                // Also ensure within 255 game limit hard cap
                if (content.size() > 255)
                    content.resize(255);
                success = true;

                LOG_INFO("playerbots", "[Ollama] {}ms bot={} reply='{}' (model={})",
                    elapsed, req.botName, content, sPlayerbotAIConfig.llmModel);

                // Update history: user + assistant
                AddHistory(req.botGuid, "user", userPrompt);
                AddHistory(req.botGuid, "assistant", content);
            }
            else
            {
                LOG_ERROR("playerbots", "[Ollama] Empty content extracted for bot {} response ({} bytes) body={}",
                    req.botName, respBody.size(), respBody.substr(0, 500));
            }
        }
        else
        {
            LOG_ERROR("playerbots", "[Ollama] No response for bot {} after {}ms (endpoint={})",
                req.botName, elapsed, endpoint);
        }

        if (!success)
        {
            content = sPlayerbotAIConfig.llmFallbackText;
            if (content.empty())
                content = "my brain hurts...";
            if (content.size() > 255)
                content.resize(255);
            LOG_INFO("playerbots", "[Ollama] Fallback for bot {}: '{}'", req.botName, content);
        }

        // Push to completed queue for world thread to deliver
        {
            std::lock_guard<std::mutex> lock(_completedMutex);
            OllamaCompletedReply comp;
            comp.botGuid = req.botGuid;
            comp.text = content;
            comp.channelSource = req.channelSource;
            comp.chanName = req.chanName;
            comp.senderName = req.senderName;
            comp.type = req.type;
            comp.senderGuidLow = req.senderGuidLow;
            comp.success = success;
            _completed.push(std::move(comp));
        }
    }

    LOG_INFO("playerbots", "[Ollama] Worker exiting");
}

void OllamaChatService::ProcessCompleted()
{
    if (!IsEnabled())
        return;

    std::queue<OllamaCompletedReply> toProcess;
    {
        std::lock_guard<std::mutex> lock(_completedMutex);
        if (_completed.empty())
            return;
        toProcess.swap(_completed);
    }

    while (!toProcess.empty())
    {
        OllamaCompletedReply comp = std::move(toProcess.front());
        toProcess.pop();

        Player* bot = ObjectAccessor::FindPlayer(comp.botGuid);
        if (!bot)
        {
            LOG_DEBUG("playerbots", "[Ollama] Bot {} not found for completed reply, dropping '{}'",
                comp.botGuid.ToString(), comp.text.substr(0, 40));
            continue;
        }

        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai)
            continue;

        // Deliver via ChatReplyAction::SendGeneralResponse logic but we need to replicate channel routing
        // For whisper: Whisper to sender
        // For say: Say
        // Use GET_PLAYERBOT_AI(bot)->GetAiObjectContext()->GetValue<time_t>("last said","chat")->Set to throttle
        // Let ChatReplyAction handle routing? Instead directly call SendGeneralResponse equivalent

        // We reuse ChatReplyAction::SendGeneralResponse style but we are in world thread, safe to call bot->Say/Whisper
        // Need to respect original type's intended output

        // Use PlayerbotAI helpers for consistency
        // Map ChatChannelSource already computed; reuse.

        // Set last said to avoid immediate spam (5-25s as original SayAction)
        // Original ChatReplyAction does Set(time+urand(5,25))
        // We do similar
        ai->GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Set(time(0) + urand(5, 25));

        bool delivered = false;
        switch (comp.channelSource)
        {
            case SRC_WHISPER:
                delivered = ai->Whisper(comp.text, comp.senderName);
                break;
            case SRC_SAY:
                delivered = ai->Say(comp.text);
                break;
            default:
                // Should not happen as we filter, but fallback to Say
                delivered = ai->Say(comp.text);
                break;
        }

        if (!delivered)
            LOG_ERROR("playerbots", "[Ollama] Failed to deliver reply for bot {}: '{}'", bot->GetName(), comp.text);
        else
            LOG_DEBUG("playerbots", "[Ollama] Delivered reply bot={} channel={} to {}: '{}'",
                bot->GetName(), (int)comp.channelSource, comp.senderName, comp.text);
    }
}
