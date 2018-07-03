// Copyright (c) 2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zmqnotificationinterface.h"
#include "zmqpublishnotifier.h"

#include "version.h"
#include "validation.h"
#include "streams.h"
#include "util.h"

void zmqError(const char *str)
{
    LogPrint("zmq", "zmq: Error: %s, errno=%s\n", str, zmq_strerror(errno));
}

CZMQNotificationInterface::CZMQNotificationInterface() : pcontext(NULL)
{
}

CZMQNotificationInterface::~CZMQNotificationInterface()
{
    Shutdown();

    for (std::list<CZMQAbstractNotifier*>::iterator it=notifiers.begin(); it!=notifiers.end(); ++it)
    {
        delete *it;
    }
}

CZMQNotificationInterface* CZMQNotificationInterface::Create()
{
    CZMQNotificationInterface* notificationInterface = NULL;
    std::map<std::string, CZMQNotifierFactory> factories;
    std::list<CZMQAbstractNotifier*> notifiers;

    factories["pubhashblock"] = CZMQAbstractNotifier::Create<CZMQPublishHashBlockNotifier>;
    factories["pubhashtx"] = CZMQAbstractNotifier::Create<CZMQPublishHashTransactionNotifier>;
    factories["pubhashtxlock"] = CZMQAbstractNotifier::Create<CZMQPublishHashTransactionLockNotifier>;
    factories["pubrawblock"] = CZMQAbstractNotifier::Create<CZMQPublishRawBlockNotifier>;
    factories["pubrawtx"] = CZMQAbstractNotifier::Create<CZMQPublishRawTransactionNotifier>;
    factories["pubrawtxlock"] = CZMQAbstractNotifier::Create<CZMQPublishRawTransactionLockNotifier>;
    factories["pubhashgovernancevote"] = CZMQAbstractNotifier::Create<CZMQPublishHashGovernanceVoteNotifier>;
    factories["pubhashgovernanceobject"] = CZMQAbstractNotifier::Create<CZMQPublishHashGovernanceObjectNotifier>;

    for (std::map<std::string, CZMQNotifierFactory>::const_iterator it=factories.begin(); it!=factories.end(); ++it)
    {
        std::string arg("-zmq" + it->first);
        if (IsArgSet(arg))
        {
            CZMQNotifierFactory factory = it->second;
            std::string address = GetArg(arg, "");
            CZMQAbstractNotifier *notifier = factory();
            notifier->SetType(it->first);
            notifier->SetAddress(address);
            notifiers.push_back(notifier);
        }
    }

    if (!notifiers.empty())
    {
        notificationInterface = new CZMQNotificationInterface();
        notificationInterface->notifiers = notifiers;

        if (!notificationInterface->Initialize())
        {
            delete notificationInterface;
            notificationInterface = NULL;
        }
    }

    return notificationInterface;
}

// Called at startup to conditionally set up ZMQ socket(s)
bool CZMQNotificationInterface::Initialize()
{
    LogPrint("zmq", "zmq: Initialize notification interface\n");
    assert(!pcontext);

    pcontext = zmq_init(1);

    if (!pcontext)
    {
        zmqError("Unable to initialize context");
        return false;
    }

    std::list<CZMQAbstractNotifier*>::iterator it=notifiers.begin();
    for (; it!=notifiers.end(); ++it)
    {
        if (it->Initialize(pcontext))
        {
            LogPrint("zmq", "  Notifier %s ready (address = %s)\n", it->GetType(), it->GetAddress());
        }
        else
        {
            LogPrint("zmq", "  Notifier %s failed (address = %s)\n", it->GetType(), it->GetAddress());
            break;
        }
    }

    if (it!=notifiers.end())
    {
        return false;
    }

    return true;
}

// Called during shutdown sequence
void CZMQNotificationInterface::Shutdown()
{
    LogPrint("zmq", "zmq: Shutdown notification interface\n");
    if (pcontext)
    {
        for (std::list<CZMQAbstractNotifier*>::iterator it=notifiers.begin(); it!=notifiers.end(); ++it)
        {
            LogPrint("zmq", "   Shutdown notifier %s at %s\n", it->GetType(), it->GetAddress());
            it->Shutdown();
        }
        zmq_ctx_destroy(pcontext);

        pcontext = 0;
    }
}

void CZMQNotificationInterface::UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload)
{
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    for (std::list<CZMQAbstractNotifier*>::iterator it = notifiers.begin(); it!=notifiers.end(); )
    {
        if (it->NotifyBlock(pindexNew))
        {
            it++;
        }
        else
        {
            it->Shutdown();
            it = notifiers.erase(i);
        }
    }
}

void CZMQNotificationInterface::SyncTransaction(const CTransaction& tx, const CBlockIndex* pindex, int posInBlock)
{
    for (std::list<CZMQAbstractNotifier*>::iterator it = notifiers.begin(); it!=notifiers.end(); )
    {
        if (it->NotifyTransaction(tx))
        {
            it++;
        }
        else
        {
            it->Shutdown();
            it = notifiers.erase(i);
        }
    }
}

void CZMQNotificationInterface::NotifyTransactionLock(const CTransaction &tx)
{
    for (std::list<CZMQAbstractNotifier*>::iterator it = notifiers.begin(); it!=notifiers.end(); )
    {
        if (it->NotifyTransactionLock(tx))
        {
            it++;
        }
        else
        {
            it->Shutdown();
            it = notifiers.erase(i);
        }
    }
}

void CZMQNotificationInterface::NotifyGovernanceVote(const CGovernanceVote &vote)
{
    for (std::list<CZMQAbstractNotifier*>::iterator it = notifiers.begin(); it != notifiers.end(); )
    {
        if (it->NotifyGovernanceVote(vote))
        {
            it++;
        }
        else
        {
            it->Shutdown();
            it = notifiers.erase(i);
        }
    }
}

void CZMQNotificationInterface::NotifyGovernanceObject(const CGovernanceObject &object)
{
    for (std::list<CZMQAbstractNotifier*>::iterator it = notifiers.begin(); it != notifiers.end(); )
    {
        if (it->NotifyGovernanceObject(object))
        {
            it++;
        }
        else
        {
            it->Shutdown();
            it = notifiers.erase(i);
        }
    }
}
