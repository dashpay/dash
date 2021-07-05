// Public Resource ℗ 2021 Đ𝖺𝗌𝗁 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#include <chainparams.h>
#include <init.h>
#include <netfulfilledman.h>
#include <util.h>

CNetFulfilledRequestManager netfulfilledman;

void CNetFulfilledRequestManager::AddFulfilledRequest(const CService& addr, const std::string& strRequest)
{
    LOCK(cs_mapFulfilledRequests);
    CService addrSquashed = Params().AllowMultiplePorts() ? addr : CService(addr, 0);
    mapFulfilledRequests[addrSquashed][strRequest] = GetTime() + Params().FulfilledRequestExpireTime();
}

bool CNetFulfilledRequestManager::HasFulfilledRequest(const CService& addr, const std::string& strRequest)
{
    LOCK(cs_mapFulfilledRequests);
    CService addrSquashed = Params().AllowMultiplePorts() ? addr : CService(addr, 0);
    fulfilledreqmap_t::iterator it = mapFulfilledRequests.find(addrSquashed);

    return  it != mapFulfilledRequests.end() &&
            it->second.find(strRequest) != it->second.end() &&
            it->second[strRequest] > GetTime();
}

void CNetFulfilledRequestManager::RemoveFulfilledRequest(const CService& addr, const std::string& strRequest)
{
    LOCK(cs_mapFulfilledRequests);
    CService addrSquashed = Params().AllowMultiplePorts() ? addr : CService(addr, 0);
    fulfilledreqmap_t::iterator it = mapFulfilledRequests.find(addrSquashed);

    if (it != mapFulfilledRequests.end()) {
        it->second.erase(strRequest);
    }
}

void CNetFulfilledRequestManager::RemoveAllFulfilledRequests(const CService& addr)
{
    LOCK(cs_mapFulfilledRequests);
    CService addrSquashed = Params().AllowMultiplePorts() ? addr : CService(addr, 0);
    fulfilledreqmap_t::iterator it = mapFulfilledRequests.find(addrSquashed);

    if (it != mapFulfilledRequests.end()) {
        mapFulfilledRequests.erase(it++);
    }
}

void CNetFulfilledRequestManager::CheckAndRemove()
{
    LOCK(cs_mapFulfilledRequests);

    int64_t now = GetTime();
    fulfilledreqmap_t::iterator it = mapFulfilledRequests.begin();

    while(it != mapFulfilledRequests.end()) {
        fulfilledreqmapentry_t::iterator it_entry = it->second.begin();
        while(it_entry != it->second.end()) {
            if(now > it_entry->second) {
                it->second.erase(it_entry++);
            } else {
                ++it_entry;
            }
        }
        if(it->second.size() == 0) {
            mapFulfilledRequests.erase(it++);
        } else {
            ++it;
        }
    }
}

void CNetFulfilledRequestManager::Clear()
{
    LOCK(cs_mapFulfilledRequests);
    mapFulfilledRequests.clear();
}

std::string CNetFulfilledRequestManager::ToString() const
{
    std::ostringstream info;
    info << "Nodes with fulfilled requests: " << (int)mapFulfilledRequests.size();
    return info.str();
}

void CNetFulfilledRequestManager::DoMaintenance()
{
    if (ShutdownRequested()) return;

    CheckAndRemove();
}
