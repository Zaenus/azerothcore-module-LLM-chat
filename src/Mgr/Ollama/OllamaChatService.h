/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_OLLAMACHATSERVICE_H
#define PLAYERBOTS_OLLAMACHATSERVICE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "ObjectGuid.h"
#include "PlayerbotAI.h"

struct OllamaPendingReply
{
    ObjectGuid botGuid;
    uint32 type = 0;
    uint32 senderGuidLow = 0;
    std::string chanName;
    std::string senderName;
    std::string botName;
    ChatChannelSource channelSource = SRC_UNDEFINED;
    std::string text;
};

struct OllamaCompletedReply
{
    ObjectGuid botGuid;
    std::string text;
    ChatChannelSource channelSource = SRC_UNDEFINED;
    std::string chanName;
    std::string senderName;
    uint32 type = 0;
    uint32 senderGuidLow = 0;
    bool success = true;
};

struct OllamaHistoryEntry
{
    std::string role; // "user" or "assistant"
    std::string content;
};

class OllamaChatService
{
public:
    static OllamaChatService& instance()
    {
        static OllamaChatService instance;
        return instance;
    }

    bool Initialize();
    void Shutdown();

    bool IsEnabled() const;
    bool ShouldUseLLM(ChatChannelSource src) const;
    bool IsRateLimited(ObjectGuid botGuid);

    // Called from ChatReplyDo on world thread (must not block)
    void EnqueueRequest(Player* bot, uint32 type, uint32 senderGuidLow, std::string const& msg,
                        std::string const& chanName, std::string const& senderName);

    // Called from world thread to drain completed replies and deliver them
    void ProcessCompleted();

    // For testing / admin
    size_t GetQueueSize();
    size_t GetHistorySize(ObjectGuid guid);

private:
    OllamaChatService() = default;
    ~OllamaChatService();
    OllamaChatService(OllamaChatService const&) = delete;
    OllamaChatService& operator=(OllamaChatService const&) = delete;

    void WorkerLoop();

    std::string BuildSystemPrompt(Player* bot);
    std::string BuildUserPrompt(std::string const& senderName, std::string const& msg, ChatChannelSource src);
    std::string EscapeJson(std::string const& s);
    std::string BuildChatJson(ObjectGuid botGuid, std::string const& systemPrompt, std::string const& userPrompt);
    std::string BuildGenerateJson(std::string const& prompt);
    std::string HttpPost(std::string const& url, std::string const& body, uint32 timeoutMs);
    std::string ExtractContentFromChatResponse(std::string const& responseBody);
    std::string ExtractContentFromGenerateResponse(std::string const& responseBody);
    std::string TrimAndLimit(std::string const& s, uint32 maxLen);
    void AddHistory(ObjectGuid botGuid, std::string const& role, std::string const& content);
    bool ParseUrl(std::string const& url, std::string& host, std::string& port, std::string& path);

    std::atomic<bool> _running{false};
    std::thread _worker;

    std::mutex _queueMutex;
    std::condition_variable _queueCv;
    std::queue<OllamaPendingReply> _queue;

    std::mutex _completedMutex;
    std::queue<OllamaCompletedReply> _completed;

    std::mutex _historyMutex;
    std::map<ObjectGuid, std::deque<OllamaHistoryEntry>> _history;

    std::mutex _rateLimitMutex;
    std::map<ObjectGuid, uint32> _lastRequestMs;

    std::string _host;
    std::string _port;
    std::string _basePath;
};

#define sOllamaChatService OllamaChatService::instance()

#endif
