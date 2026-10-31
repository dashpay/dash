// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/exception.h>

#include <logging.h>
#include <stacktraces.h>
#include <tinyformat.h>

#include <exception>
#include <iostream>
#include <string>

void PrintExceptionContinue(const std::exception_ptr pex, std::string_view thread_name)
{
    std::string message = strprintf("\"%s\" raised an exception\n%s", thread_name, GetPrettyExceptionStr(pex));
    LogPrintf("\n\n************************\n%s\n", message);
    tfm::format(std::cerr, "\n\n************************\n%s\n", message);
}
