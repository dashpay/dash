// Copyright (c) 2017-2021 Vincent Thiery
// Copyirght (C) 2017-2021 The cpp-statsd-client contributors
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <analytics/sdclient.h>

#include <util/system.h>
#include <statsd_client.h>

namespace Statsd {
StatsdClient::StatsdClient(const std::string& host,
                           const uint16_t port,
                           const std::string& prefix,
                           const std::string& nspace,
                           const uint64_t batchsize,
                           const uint64_t sendInterval,
                           const unsigned int gaugePrecision) noexcept
    : m_prefix{detail::sanitizePrefix(prefix)},
      m_nspace{detail::sanitizePrefix(nspace)},
      m_sender{std::make_unique<UDPSender>(host, port, batchsize, sendInterval)},
      m_gaugePrecision(gaugePrecision)
{
    // Avoid re-allocations by reserving a generous buffer
    m_buffer.reserve(256);
}

const std::string StatsdClient::errorMessage() const noexcept {
    return m_sender->errorMessage();
}

void StatsdClient::dec(const std::string& key,
                       float frequency,
                       const std::vector<std::string>& tags) const noexcept {
    count(key, -1, frequency, tags);
}

void StatsdClient::inc(const std::string& key,
                       float frequency,
                       const std::vector<std::string>& tags) const noexcept {
    count(key, 1, frequency, tags);
}

void StatsdClient::count(const std::string& key,
                         const int delta,
                         float frequency,
                         const std::vector<std::string>& tags) const noexcept {
    send(key, delta, detail::METRIC_TYPE_COUNT, frequency, tags);
}

void StatsdClient::timing(const std::string& key,
                          const unsigned int ms,
                          float frequency,
                          const std::vector<std::string>& tags) const noexcept {
    send(key, ms, detail::METRIC_TYPE_TIMING, frequency, tags);
}

void StatsdClient::set(const std::string& key,
                       const unsigned int sum,
                       float frequency,
                       const std::vector<std::string>& tags) const noexcept {
    send(key, sum, detail::METRIC_TYPE_SET, frequency, tags);
}
}  // namespace Statsd

static std::unique_ptr<Statsd::StatsdClient> g_stats_agent{nullptr};

bool InitStatsAgent(const ArgsManager& args) {
    if (g_stats_agent) {
        return false;
    }

    g_stats_agent = std::make_unique<Statsd::StatsdClient>(
        args.GetArg("-statshost",     DEFAULT_STATSD_HOST),
        args.GetArg("-statsport",     DEFAULT_STATSD_PORT),
        args.GetArg("-statshostname", DEFAULT_STATSD_HOSTNAME),
        args.GetArg("-statsns",       DEFAULT_STATSD_NAMESPACE),
        /* batchsize */ 0,
        std::min(
            std::max(
                args.GetArg("-statsperiod", DEFAULT_STATSD_PERIOD),
                MIN_STATSD_PERIOD
            ),
            MAX_STATSD_PERIOD
        )  * 1000, /* gaugePrecision */ 4
    );

    if (g_stats_agent) {
        if (g_stats_agent->IsConnected()) {
            return true;
        }
    }

    g_stats_agent.reset();
    return false;
}

void StopStatsAgent() {
    if (!g_stats_agent) {
        return;
    }
    g_stats_agent->flush();
    g_stats_agent.reset();
    return;
}
