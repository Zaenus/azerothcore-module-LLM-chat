/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option), any later version.
 */

#include "MemoryMgr.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"
#include "QueryResult.h"

void MemoryMgr::AddHistory(uint32 botGuid, uint32 playerGuid, std::string const& role, std::string const& content)
{
    if (content.empty())
        return;

    std::lock_guard<std::mutex> lock(m_lock);
    auto& dq = m_histories[botGuid];
    ChatHistoryEntry e;
    e.playerGuid = playerGuid;
    e.role = role;
    e.content = content;
    e.timestamp = time(nullptr);
    dq.push_back(std::move(e));

    uint32 limit = sPlayerbotAIConfig.llmHistorySize ? sPlayerbotAIConfig.llmHistorySize * 2 : 10; // *2 for user+assistant
    while (dq.size() > limit)
        dq.pop_front();
}

std::deque<ChatHistoryEntry> MemoryMgr::GetHistory(uint32 botGuid)
{
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = m_histories.find(botGuid);
    if (it != m_histories.end())
        return it->second;
    return {};
}

std::vector<std::string> MemoryMgr::GetMemories(uint32 botGuid, uint32 playerGuid)
{
    std::vector<std::string> out;
    if (!sPlayerbotAIConfig.llmMemoryEnabled)
        return out;

    // Forever memory: fetch most recent 5 for this bot+player, forever retention
    // Direct query; worker thread will call this, so blocking is okay
    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT content FROM playerbots_llm_memory WHERE bot_guid = {} AND player_guid = {} ORDER BY id DESC LIMIT 5",
        botGuid, playerGuid);

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            std::string c = fields[0].Get<std::string>();
            if (!c.empty())
                out.push_back(std::move(c));
        } while (result->NextRow());
        // reverse to chronological for prompt
        std::reverse(out.begin(), out.end());
    }
    return out;
}

void MemoryMgr::StoreMemory(uint32 botGuid, uint32 playerGuid, std::string const& content)
{
    if (!sPlayerbotAIConfig.llmMemoryEnabled || content.empty())
        return;

    std::string truncated = content;
    if (truncated.size() > 500)
        truncated.resize(500);

    std::string escaped = truncated;
    // Escape for SQL: backslash, single quote, double quote
    size_t pos = 0;
    while ((pos = escaped.find('\\', pos)) != std::string::npos)
    {
        escaped.replace(pos, 1, "\\\\");
        pos += 2;
    }
    pos = 0;
    while ((pos = escaped.find('\'', pos)) != std::string::npos)
    {
        escaped.replace(pos, 1, "\\'");
        pos += 2;
    }
    pos = 0;
    while ((pos = escaped.find('"', pos)) != std::string::npos)
    {
        escaped.replace(pos, 1, "\\\"");
        pos += 2;
    }

    // Forever: never delete, just insert
    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbots_llm_memory (bot_guid, player_guid, content) VALUES ({}, {}, \"{}\")",
        botGuid, playerGuid, escaped);
}

void MemoryMgr::StoreExchange(uint32 botGuid, uint32 playerGuid, std::string const& playerName, std::string const& userMsg, std::string const& botReply)
{
    // Store as single memory entry summarizing the exchange
    std::string mem = playerName + ": " + userMsg;
    if (!botReply.empty())
    {
        mem += " | " + botReply;
        if (mem.size() > 500)
            mem.resize(500);
    }

    // Also keep short history for prompt context
    AddHistory(botGuid, playerGuid, "user", userMsg);
    AddHistory(botGuid, playerGuid, "assistant", botReply);

    // Check per-player limit for warning (soft cap); we still store forever but log
    // Could add summarization here if count > LLMMemoryPerPlayerLimit
    StoreMemory(botGuid, playerGuid, mem);
}
