#ifndef TESTLOGGER_H
#define TESTLOGGER_H

#include <cassert>
#include <tuple>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

template <size_t I>
struct getarg_impl
{
    template <typename T>
    static std::string get(size_t idx, const T& tup)
    {
        std::ostringstream res;

        if (idx == I-1)
            res << std::get<I-1>(tup);
        else
            res << getarg_impl<I-1>::get(idx, tup);
        return res.str();
    }
};

template <>
struct getarg_impl<0>
{
    template <typename T>
    static std::string get(size_t idx, const T& tup) { assert(false); }
};

template <typename... Args>
std::string GetArgByIndex(size_t idx, const Args&... args)
{
    return getarg_impl<sizeof...(Args)>::get(idx, std::forward_as_tuple(args...));
}


struct TestFormat
{
    template<typename... T>
    TestFormat(const char* _event_name, T... _indices) : event_name(_event_name), params{_indices...} {}

    const char* event_name;
    std::vector<int> params;
};

class CTestLogger
{
    const std::map<std::string, TestFormat>& mapMessages;

    int fdLog;
    std::string pipeName;
    std::mutex mutex;

public:
    CTestLogger(const std::map<std::string, TestFormat>& _mapTestMessages) : mapMessages(_mapTestMessages)
    {
        fdLog = -1;
    }

    ~CTestLogger()
    {
        if (fdLog != -1)
            close(fdLog);
    }

    void Initialize(std::string _pipeName)
    {
        std::lock_guard<std::mutex> lock(mutex);
        pipeName = _pipeName;
    }

    static CTestLogger& GetInstance();

    template<typename... Args>
    void SendMsg(const char* format, const Args&... args)
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (fdLog == -1 && !pipeName.empty())
#ifdef WIN32
            //TODO: untested code needs to be verified and refactored if necessary
            fdLog = open(pipeName.c_str(), 1 | 04000);
#else
            fdLog = open(pipeName.c_str(), O_WRONLY | O_NONBLOCK);
#endif

        if (fdLog == -1)
            return;

        auto it = mapMessages.find(format);
        if (it != mapMessages.end())
        {
            std::ostringstream msg;

            msg << it->second.event_name;
            for(const auto& i : it->second.params)
                msg << ":" << GetArgByIndex(i, args...);
            msg << "\n";

            if (write(fdLog, msg.str().c_str(), msg.str().length()) == -1) {
                close(fdLog);
                fdLog = -1;
            }
        }
    }

};


#endif // TESTLOGGER_H
