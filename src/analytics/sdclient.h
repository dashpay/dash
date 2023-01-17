// Copyright (c) 2017-2021 Vincent Thiery
// Copyirght (C) 2017-2021 The cpp-statsd-client contributors
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ANALYTICS_SDCLIENT_H
#define BITCOIN_ANALYTICS_SDCLIENT_H

#include <analytics/sender.h>

#include <random.h>

#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace Statsd {
class StatsdClient {
public:
    StatsdClient(const std::string& host,
                 const uint16_t port,
                 const std::string& prefix,
                 const uint64_t batchsize = 0,
                 const uint64_t sendInterval = 1000,
                 const unsigned int gaugePrecision = 4) noexcept;

    StatsdClient(const StatsdClient&) = delete;
    StatsdClient& operator=(const StatsdClient&) = delete;

    //! Returns the error message as an std::string
    const std::string errorMessage() const noexcept;
    //! Increments the key, at a given frequency rate
    void inc(const std::string& key,
             float frequency = 1.0f,
             const std::vector<std::string>& tags = {}) const noexcept;
    //! Increments the key, at a given frequency rate
    void dec(const std::string& key,
             float frequency = 1.0f,
             const std::vector<std::string>& tags = {}) const noexcept;
    //! Adjusts the specified key by a given delta, at a given frequency rate
    void count(const std::string& key,
               const int delta,
               float frequency = 1.0f,
               const std::vector<std::string>& tags = {}) const noexcept;
    //! Records a gauge for the key, with a given value, at a given frequency rate
    template <typename T>
    void gauge(const std::string& key,
               const T value,
               float frequency = 1.0f,
               const std::vector<std::string>& tags = {}) const noexcept;
    //! Records a timing for a key, at a given frequency
    void timing(const std::string& key,
                const unsigned int ms,
                float frequency = 1.0f,
                const std::vector<std::string>& tags = {}) const noexcept;
    //! Records a count of unique occurrences for a key, at a given frequency
    void set(const std::string& key,
             const unsigned int sum,
             float frequency = 1.0f,
             const std::vector<std::string>& tags = {}) const noexcept;
    //! Flush any queued stats to the daemon
    void flush() noexcept { m_sender->flush(); }
    //! Returns true if UDP socket has been successfully established
    bool IsConnected() noexcept { return m_sender->initialized(); }
private:
    //! Send a value for a key, according to its type, at a given frequency
    template <typename T>
    void send(const std::string& key,
              const T value,
              const char* type,
              float frequency,
              const std::vector<std::string>& tags) const noexcept;

private:
    //! The prefix to be used for metrics
    std::string m_prefix;
    //! The UDP sender to be used for actual sending
    const std::unique_ptr<UDPSender> m_sender;
    //! The buffer string format our stats before sending them
    mutable std::string m_buffer;
    //! Fixed floating point precision of gauges
    unsigned int m_gaugePrecision;
};

namespace detail {
inline std::string sanitizePrefix(std::string prefix) {
    // For convenience we provide the dot when generating the stat message
    if (!prefix.empty() && prefix.back() == '.') {
        prefix.pop_back();
    }
    return prefix;
}

// All supported metric types
constexpr char METRIC_TYPE_COUNT[] = "c";
constexpr char METRIC_TYPE_GAUGE[] = "g";
constexpr char METRIC_TYPE_TIMING[] = "ms";
constexpr char METRIC_TYPE_SET[] = "s";
}  // namespace detail

template <typename T>
inline void StatsdClient::gauge(const std::string& key,
                                const T value,
                                const float frequency,
                                const std::vector<std::string>& tags) const noexcept {
    send(key, value, detail::METRIC_TYPE_GAUGE, frequency, tags);
}

template <typename T>
void StatsdClient::send(const std::string& key,
                        const T value,
                        const char* type,
                        float frequency,
                        const std::vector<std::string>& tags) const noexcept {
    // Bail if we can't send anything anyway
    if (!m_sender->initialized()) {
        return;
    }

    // A valid frequency is: 0 <= f <= 1
    // At 0 you never emit the stat, at 1 you always emit the stat and with anything else you roll the dice
    FastRandomContext m_randomEngine;
    frequency = std::max(std::min(frequency, 1.f), 0.f);
    constexpr float epsilon{0.0001f};
    const bool isFrequencyOne = std::fabs(frequency - 1.0f) < epsilon;
    const bool isFrequencyZero = std::fabs(frequency) < epsilon;
    if (isFrequencyZero ||
       (!isFrequencyOne && (frequency < float(m_randomEngine(std::numeric_limits<uint32_t>::max())) / float(std::numeric_limits<uint32_t>::max())))) {
        return;
    }

    // Format the stat message
    std::stringstream valueStream;
    valueStream << std::fixed << std::setprecision(m_gaugePrecision) << value;

    m_buffer.clear();

    m_buffer.append(m_prefix);
    if (!m_prefix.empty() && !key.empty()) {
        m_buffer.push_back('.');
    }

    m_buffer.append(key);
    m_buffer.push_back(':');
    m_buffer.append(valueStream.str());
    m_buffer.push_back('|');
    m_buffer.append(type);

    if (frequency < 1.f) {
        m_buffer.append("|@0.");
        m_buffer.append(std::to_string(static_cast<int>(frequency * 100)));
    }

    if (!tags.empty()) {
        m_buffer.append("|#");
        for (const auto& tag : tags) {
            m_buffer.append(tag);
            m_buffer.push_back(',');
        }
        m_buffer.pop_back();
    }

    // Send the message via the UDP sender
    m_sender->send(m_buffer);
}
}  // namespace Statsd

#endif // BITCOIN_ANALYTICS_SDCLIENT_H
