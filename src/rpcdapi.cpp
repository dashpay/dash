// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Dash developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "db.h"
#include "init.h"
#include "rpcserver.h"
#include "utilmoneystr.h"
#include "evo/dapi.h"
// #include "evo/file.h"

#include "util.h"

#include <fstream>
using namespace json_spirit;
using namespace std;

/*
    More Information About DAPI

    This uses a simple json interface to execute commands on the network through DAPI.
    To run a daemon and take commands from the network, you will also need to run the server in
    the dash-t2 package.

*/


Value dapi(const Array& params, bool fHelp)
{

    if (fHelp || params.size() < 1)
        throw runtime_error(
                "dapi \"json_command\"\n"
                "Execute a command via DAPI\n"
                );

    /*
     

        Executing web events:

        dashd --datadir=example --eventnotify="/Users/evan/Desktop/dash-2t/serve/event.py --event=%" --daemon
        dash-cli dapi "{\"txid\":\"myid\",\"aeou\":\"123\",\"vout\":\"0\"}" "{\"address\":0.01}"

        dash_t2 --eventnotify="/Users/evan/Desktop/dash-2t/serve/event.py --event=%"
    */

    std::string strCommand = params[0].get_str();
    json_spirit::Value val;

    printf("%s\n", strCommand.c_str());

    bool fSuccess = json_spirit::read_string(strCommand, val);
    if (fSuccess) {
        Object obj = val.get_obj();
        CDAPI::Execute(obj);
        return "ok";
    }

    //TODO: add better errors
    return "parse error";
}

Value dapif(const Array& params, bool fHelp)
{

    if (fHelp || params.size() < 1)
        throw runtime_error(
                "dapif \"json_file\"\n"
                "Execute a command via DAPI\n"
                );

    /*
        Executing web events:

        dashd --datadir=example --eventnotify="/Users/evan/Desktop/dash-2t/serve/event.py --event=%" --daemon
        dash-cli dapi "{\"txid\":\"myid\",\"aeou\":\"123\",\"vout\":\"0\"}" "{\"address\":0.01}"

        dash_t2 --datadir2=/Users/evan/.dash-data/ --eventnotify="/Users/evan/Desktop/dash-2t/serve/event.py --event=%"
    */

    std::string strPath = params[0].get_str();
    std::ifstream t(strPath.c_str());
    std::string str((std::istreambuf_iterator<char>(t)),
                     std::istreambuf_iterator<char>());

    printf("%s\n", str.c_str());

    json_spirit::Value val;
    
    bool fSuccess = json_spirit::read_string(str, val);
    if (fSuccess) {
        Object obj = val.get_obj();
        CDAPI::Execute(obj);
        return "ok";
    }

    //TODO: add better errors
    return "parse error";
}
