/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option), any later version.
 */

#ifndef PLAYERBOTS_PERSONALITYMGR_H
#define PLAYERBOTS_PERSONALITYMGR_H

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common.h"

struct PersonalityEntry
{
    uint8 id = 0;
    std::string name;
    std::string suffix;
};

class PersonalityMgr
{
public:
    static PersonalityMgr& instance()
    {
        static PersonalityMgr instance;
        return instance;
    }

    void Load();
    std::string GetPersonalitySuffix(uint32 botGuid, uint8 race, uint8 cls);
    uint8 GetPersonalityId(uint32 botGuid, uint8 race, uint8 cls);
    bool HasPersonality(uint32 botGuid) const;

private:
    PersonalityMgr() = default;
    ~PersonalityMgr() = default;
    PersonalityMgr(PersonalityMgr const&) = delete;
    PersonalityMgr& operator=(PersonalityMgr const&) = delete;

    uint8 PickDeterministic(uint32 botGuid, uint8 race, uint8 cls);
    void EnsureAssigned(uint32 botGuid, uint8 race, uint8 cls);
    bool IsRaceClassFit(uint8 personalityId, uint8 race, uint8 cls) const;

    std::mutex m_lock;
    std::unordered_map<uint8, PersonalityEntry> m_personalities; // id -> entry
    std::unordered_map<uint32, uint8> m_botPersonality;          // botGuid -> personalityId
    bool m_loaded = false;
};

#define sPersonalityMgr PersonalityMgr::instance()

#endif
