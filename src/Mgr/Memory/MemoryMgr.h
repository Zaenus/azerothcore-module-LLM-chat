/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option), any later version.
 */

#ifndef PLAYERBOTS_MEMORYMGR_H
#define PLAYERBOTS_MEMORYMGR_H

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common.h"

struct ChatHistoryEntry
{
    uint32 playerGuid = 0;
    std::string role;    // "user" or "assistant"
    std::string content;
    time_t timestamp = 0;
};

class MemoryMgr
{
public:
    static MemoryMgr& instance()
    {
        static MemoryMgr instance;
        return instance;
    }

    void AddHistory(uint32 botGuid, uint32 playerGuid, std::string const& role, std::string const& content);
    std::deque<ChatHistoryEntry> GetHistory(uint32 botGuid);
    std::vector<std::string> GetMemories(uint32 botGuid, uint32 playerGuid);
    void StoreMemory(uint32 botGuid, uint32 playerGuid, std::string const& content);
    void StoreExchange(uint32 botGuid, uint32 playerGuid, std::string const& playerName, std::string const& userMsg, std::string const& botReply);

private:
    MemoryMgr() = default;
    ~MemoryMgr() = default;
    MemoryMgr(MemoryMgr const&) = delete;
    MemoryMgr& operator=(MemoryMgr const&) = delete;

    std::mutex m_lock;
    std::unordered_map<uint32, std::deque<ChatHistoryEntry>> m_histories; // botGuid -> deque
};

#define sMemoryMgr MemoryMgr::instance()

#endif
