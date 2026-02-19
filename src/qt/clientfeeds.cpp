// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/clientfeeds.h>

#include <util/threadnames.h>

#include <QDebug>
#include <QThread>

FeedBase::FeedBase(QObject* parent, const FeedBase::Config& config) :
    QObject{parent},
    m_config{config}
{
}

FeedBase::~FeedBase() = default;

void FeedBase::requestForceRefresh()
{
    if (m_timer) {
        m_timer->start(0);
    }
}

void FeedBase::requestRefresh()
{
    if (m_timer && !m_timer->isActive()) {
        m_timer->start(m_syncing.load() ? m_config.m_throttle : m_config.m_baseline);
    }
}

ClientFeeds::ClientFeeds(QObject* parent) :
    QObject{parent},
    m_thread{new QThread(this)}
{
}

ClientFeeds::~ClientFeeds()
{
    stop();
}

void ClientFeeds::registerFeed(FeedBase* raw)
{
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    raw->m_timer = timer;

    connect(timer, &QTimer::timeout, this, [this, raw] {
        if (raw->m_in_progress.exchange(true)) {
            raw->m_retry_pending.store(true);
            return;
        }
        QMetaObject::invokeMethod(m_worker, [this, raw] {
            try {
                raw->fetch();
            } catch (const std::exception& e) {
                qWarning() << "ClientFeeds::fetch() exception: " << e.what();
            } catch (...) {
                qWarning() << "ClientFeeds::fetch() unknown exception";
            }
            QTimer::singleShot(0, raw, [this, raw] {
                raw->m_in_progress.store(false);
                if (m_stopped) return;
                Q_EMIT raw->dataReady();
                if (raw->m_retry_pending.exchange(false)) {
                    raw->requestRefresh();
                }
            });
        });
    });
}

void ClientFeeds::start()
{
    m_worker = new QObject();
    m_worker->moveToThread(m_thread);
    m_thread->start();
    QMetaObject::invokeMethod(m_worker, [] { util::ThreadRename("qt-clientfeed"); });

    for (const auto& source : m_sources) {
        if (source->m_timer) {
            source->m_timer->start(0);
        }
    }
}

void ClientFeeds::stop()
{
    if (m_stopped) {
        return;
    }

    m_stopped = true;
    for (const auto& source : m_sources) {
        if (source->m_timer) {
            source->m_timer->stop();
            source->m_timer = nullptr;
        }
    }
    if (m_thread->isRunning()) {
        m_thread->quit();
        m_thread->wait();
    }
    delete m_worker;
    m_worker = nullptr;
}

void ClientFeeds::setSyncing(bool syncing)
{
    if (m_stopped) {
        return;
    }

    for (const auto& source : m_sources) {
        if (source->m_syncing.load() == syncing) {
            continue;
        }
        source->setSyncing(syncing);
        if (source->m_timer && source->m_timer->isActive()) {
            source->m_timer->start(syncing ? source->m_config.m_throttle : source->m_config.m_baseline);
        }
    }
}
