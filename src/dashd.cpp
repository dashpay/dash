// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2019 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/dash-config.h>
#endif

#include <chainparams.h>
#include <clientversion.h>
#include <compat.h>
#include <fs.h>
#include <rpc/server.h>
#include <init.h>
#include <noui.h>
#include <util.h>
#include <httpserver.h>
#include <httprpc.h>
#include <utilstrencodings.h>
#include <walletinitinterface.h>
#include <stacktraces.h>

#include <boost/thread.hpp>

#include <stdio.h>

/* Introduction text for doxygen: */

/*! \mainpage Developer documentation
 *
 * \section intro_sec Introduction
 *
 * This is the developer documentation of the reference client for an experimental new digital currency called Dash (https://www.dash.org/),
 * which enables instant payments to anyone, anywhere in the world. Dash uses peer-to-peer technology to operate
 * with no central authority: managing transactions and issuing money are carried out collectively by the network.
 *
 * The software is a community-driven open source project, released under the MIT license.
 *
 * \section Navigation
 * Use the buttons <code>Namespaces</code>, <code>Classes</code> or <code>Files</code> at the top of the page to start navigating the code.
 */

static void WaitForShutdown()
{
    while (!ShutdownRequested())
    {
        MilliSleep(200);
    }
    Interrupt();
}

//////////////////////////////////////////////////////////////////////////////
//
// Start
//
static bool AppInit(int argc, char* argv[])
{
    bool fRet = false;

    //
    // Parameters
    //
    // If Qt is used, parameters/bitcoin.conf are parsed in qt/bitcoin.cpp's main()
    SetupServerArgs();
#if HAVE_DECL_DAEMON
    gArgs.AddArg("-daemon", _("Run in the background as a daemon and accept commands"), false, OptionsCategory::OPTIONS);
#endif
    gArgs.ParseParameters(argc, argv);

    if (gArgs.IsArgSet("-printcrashinfo")) {
        std::cout << GetCrashInfoStrFromSerializedStr(gArgs.GetArg("-printcrashinfo", "")) << std::endl;
        return true;
    }

    // Process help and version before taking care about datadir
    if (HelpRequested(gArgs) || gArgs.IsArgSet("-version")) {
        std::string strUsage = strprintf(_("%s Daemon"), _(PACKAGE_NAME)) + " " + _("version") + " " + FormatFullVersion() + "\n";

        if (gArgs.IsArgSet("-version"))
        {
            strUsage += FormatParagraph(LicenseInfo());
        }
        else
        {
            strUsage += "\n" + _("Usage:") + "\n" +
                  "  dashd [options]                     " + strprintf(_("Start %s Daemon"), _(PACKAGE_NAME)) + "\n";

            strUsage += "\n" + gArgs.GetHelpMessage();
        }

        fprintf(stdout, "%s", strUsage.c_str());
        return true;
    }

    try
    {
        bool datadirFromCmdLine = gArgs.IsArgSet("-datadir");
        if (datadirFromCmdLine && !fs::is_directory(GetDataDir(false)))
        {
            fprintf(stderr, "Error: Specified data directory \"%s\" does not exist.\n", gArgs.GetArg("-datadir", "").c_str());
            return false;
        }
        try
        {
            gArgs.ReadConfigFiles();
        } catch (const std::exception& e) {
            fprintf(stderr,"Error reading configuration file: %s\n", e.what());
            return false;
        }
        if (!datadirFromCmdLine && !fs::is_directory(GetDataDir(false)))
        {
            fprintf(stderr, "Error: Specified data directory \"%s\" from config file does not exist.\n", gArgs.GetArg("-datadir", "").c_str());
            return EXIT_FAILURE;
        }
        // Check for -testnet or -regtest parameter (Params() calls are only valid after this clause)
        try {
            SelectParams(gArgs.GetChainName());
        } catch (const std::exception& e) {
            fprintf(stderr, "Error: %s\n", e.what());
            return false;
        }

        // Error out when loose non-argument tokens are encountered on command line
        for (int i = 1; i < argc; i++) {
            if (!IsSwitchChar(argv[i][0])) {
                fprintf(stderr, "Error: Command line contains unexpected token '%s', see dashd -h for a list of options.\n", argv[i]);
                return false;
            }
        }

        // -server defaults to true for dashd but not for the GUI so do this here
        gArgs.SoftSetBoolArg("-server", true);
        // Set this early so that parameter interactions go to console
        InitLogging();
        InitParameterInteraction();
        if (!AppInitBasicSetup())
        {
            // InitError will have been called with detailed error, which ends up on console
            return false;
        }
        if (!AppInitParameterInteraction())
        {
            // InitError will have been called with detailed error, which ends up on console
            return false;
        }
        if (!AppInitSanityChecks())
        {
            // InitError will have been called with detailed error, which ends up on console
            return false;
        }
        if (gArgs.GetBoolArg("-daemon", false))
        {
#if HAVE_DECL_DAEMON
            fprintf(stdout, "Dash Core server starting\n");

            // Daemonize
            if (daemon(1, 0)) { // don't chdir (1), do close FDs (0)
                fprintf(stderr, "Error: daemon() failed: %s\n", strerror(errno));
                return false;
            }
#else
            fprintf(stderr, "Error: -daemon is not supported on this operating system\n");
            return false;
#endif // HAVE_DECL_DAEMON
        }
        // Lock data directory after daemonization
        if (!AppInitLockDataDirectory())
        {
            // If locking the data directory failed, exit immediately
            return false;
        }
        fRet = AppInitMain();
    } catch (...) {
        PrintExceptionContinue(std::current_exception(), "AppInit()");
    }

    if (!fRet)
    {
        Interrupt();
    } else {
        WaitForShutdown();
    }
    Shutdown();

    return fRet;
}

int main(int argc, char* argv[])
{
    RegisterPrettyTerminateHander();
    RegisterPrettySignalHandlers();

    SetupEnvironment();

    // Connect dashd signal handlers
    noui_connect();

    return (AppInit(argc, argv) ? EXIT_SUCCESS : EXIT_FAILURE);
}
