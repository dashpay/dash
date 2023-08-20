// Copyright (c) 2017-2021 Vincent Thiery
// Copyirght (C) 2017-2021 The cpp-statsd-client contributors
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <analytics/sdclient.h>

namespace Statsd {
StatsdClient::StatsdClient(const std::string& host,
                           const uint16_t port,
                           const std::string& prefix,
                           const uint64_t batchsize,
                           const uint64_t sendInterval,
                           const unsigned int gaugePrecision) noexcept
    : m_prefix{detail::sanitizePrefix(prefix)},
      m_sender{std::make_unique<UDPSender>(host, port, batchsize, sendInterval)},
      m_gaugePrecision(gaugePrecision)
{
    // Initialize the random generator to be used for sampling
    seed();
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
