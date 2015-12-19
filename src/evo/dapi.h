

// Copyright (c) 2014-2015 The Dash developers

// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#ifndef DAPI_H
#define DAPI_H

#include "main.h"
#include "db.h"
#include "init.h"
#include "rpcserver.h"

#include "util.h"
#include "file.h"
#include "json/json_spirit.h"

#include <stdint.h>

#include "json/json_spirit_value.h"
#include "univalue/univalue.h"
#include <string>
#include <sstream>

#include <boost/lexical_cast.hpp>

using namespace std;
using namespace json_spirit;

std::string GetProfileFile(std::string strUID)
{
    boost::filesystem::path filename = GetDataDirectory() / "users" / strUID;
    return filename.c_str();
}

std::string GetPrivateDataFile(std::string strUID, int nSlot)
{
    std::string strFilename = strUID + "." + boost::lexical_cast<std::string>(nSlot);
    boost::filesystem::path filename = GetDataDirectory() / "users" / strFilename;
    return filename.c_str();
}


std::string escapeJsonString(const std::string& input) {
    std::ostringstream ss;
    //for (auto iter = input.cbegin(); iter != input.cend(); iter++) {
    //C++98/03:
    for (std::string::const_iterator iter = input.begin(); iter != input.end(); iter++) {
        switch (*iter) {
            case '\\': ss << "\\\\"; break;
            case '"': ss << "\\\""; break;
            case '/': ss << "\\/"; break;
            case '\b': ss << "\\b"; break;
            case '\f': ss << "\\f"; break;
            case '\n': ss << "\\n"; break;
            case '\r': ss << "\\r"; break;
            case '\t': ss << "\\t"; break;
            default: ss << *iter; break;
        }
    }
    return ss.str();
}

class CDAPI
{
private:
    CDAPI();

public:
    static bool Execute(Object& obj);
    static bool ValidateSignature(Object& obj);
    static bool GetProfile(Object& obj);
    static bool SetProfile(Object& obj);
    static bool GetPrivateData(Object& obj);
    static bool SetPrivateData(Object& obj);
    static bool SendMessage(Object& obj);
    static bool BroadcastMessage(Object& obj);

};

#endif