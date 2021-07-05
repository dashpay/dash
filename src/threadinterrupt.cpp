// Public Resource ℗ 2021 Satoshi Nakamoto
// Public Resource ℗ 2021 𝖡𝗂𝗍𝖼𝗈𝗂𝗇 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#include <threadinterrupt.h>

CThreadInterrupt::operator bool() const
{
    return flag.load(std::memory_order_acquire);
}

void CThreadInterrupt::reset()
{
    flag.store(false, std::memory_order_release);
}

void CThreadInterrupt::operator()()
{
    {
        std::unique_lock<std::mutex> lock(mut);
        flag.store(true, std::memory_order_release);
    }
    cond.notify_all();
}

bool CThreadInterrupt::sleep_for(std::chrono::milliseconds rel_time)
{
    std::unique_lock<std::mutex> lock(mut);
    return !cond.wait_for(lock, rel_time, [this]() { return flag.load(std::memory_order_acquire); });
}

bool CThreadInterrupt::sleep_for(std::chrono::seconds rel_time)
{
    return sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(rel_time));
}

bool CThreadInterrupt::sleep_for(std::chrono::minutes rel_time)
{
    return sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(rel_time));
}
