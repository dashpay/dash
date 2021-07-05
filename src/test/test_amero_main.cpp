// Public Resource ℗ 2021 𝖡𝗂𝗍𝖼𝗈𝗂𝗇 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#define BOOST_TEST_MODULE Amero Test Suite

#include <net.h>
#include <stacktraces.h>

#include <memory>

#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_monitor.hpp>

std::unique_ptr<CConnman> g_connman;

[[noreturn]] void Shutdown(void* parg)
{
  std::exit(EXIT_SUCCESS);
}

[[noreturn]] void StartShutdown()
{
  std::exit(EXIT_SUCCESS);
}

bool ShutdownRequested()
{
  return false;
}

template<typename T>
void translate_exception(const T &e)
{
    std::cerr << GetPrettyExceptionStr(std::current_exception()) << std::endl;
    throw;
}

template<typename T>
void register_exception_translator()
{
    boost::unit_test::unit_test_monitor.register_exception_translator<T>(&translate_exception<T>);
}

struct ExceptionInitializer {
    ExceptionInitializer()
    {
        RegisterPrettyTerminateHander();
        RegisterPrettySignalHandlers();

        register_exception_translator<std::exception>();
        register_exception_translator<std::string>();
        register_exception_translator<const char*>();
    }
    ~ExceptionInitializer()
    {
    }
};

BOOST_GLOBAL_FIXTURE(ExceptionInitializer);
