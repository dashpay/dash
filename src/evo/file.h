

#ifndef DASHDRIVE_FILE_H
#define DASHDRIVE_FILE_H

#include "init.h"
#include "base58.h"
#include "addrman.h"
#include "amount.h"
#include "checkpoints.h"
#include "compat/sanity.h"
#include "key.h"
#include "main.h"

#include <stdint.h>
#include <stdio.h>
#include <fstream>
#include <string>
#include <streambuf>

#include "json/json_spirit.h"
#include "json/json_spirit_value.h"
#include "json/json_spirit_writer.h"

#include <boost/filesystem.hpp>
#include "univalue/univalue.h"


using namespace json_spirit;
using namespace std;


// TODO: What include is required for this?
#define CLIENT_VERSION 1

boost::filesystem::path GetDataDirectory()
{
    string strDataDir2 = GetArg("-datadir2", "");
    if(strDataDir2 != "") return strDataDir2;

    namespace fs = boost::filesystem;
    // Windows < Vista: C:\Documents and Settings\Username\Application Data\Dash
    // Windows >= Vista: C:\Users\Username\AppData\Roaming\Dash
    // Mac: ~/Library/Application Support/Dash
    // Unix: ~/.dash
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "Dash";
#else
    fs::path pathRet;
    char* pszHome = getenv("HOME");
    if (pszHome == NULL || strlen(pszHome) == 0)
        pathRet = fs::path("/");
    else
        pathRet = fs::path(pszHome);
#ifdef MAC_OSX
    // Mac
    pathRet /= "Library/Application Support";
    TryCreateDirectory(pathRet);
    return pathRet / "DashData";
#else
    // Unix
    return pathRet / ".dash-data";
#endif
#endif
}

class CDriveFile
{
private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;
    string strMagicMessage;
    string strPath;
    bool fDirty;

public:
    Object obj;

    enum ReadResult {
        Ok,
        FileError,
        HashReadError,
        IncorrectHash,
        IncorrectMagicMessage,
        IncorrectMagicNumber,
        IncorrectFormat
    };

    CDriveFile();
    CDriveFile(const string strPathIn);

    bool Exists()
    {
        if ( boost::filesystem::exists( strPath ) )
        {
            return true;
        }

        return false;
    }

    ReadResult Read()    
    {
        std::ifstream t(strPath.c_str());
        std::string str((std::istreambuf_iterator<char>(t)),
                         std::istreambuf_iterator<char>());

        json_spirit::Value val;

        bool fSuccess = json_spirit::read(str, val);
        if (fSuccess) {
            obj = val.get_obj();
            return Ok;
        }

        return FileError;
    }

    bool Write()
    {
        LOCK(cs);

        ofstream os( strPath.c_str() );
        json_spirit::write( obj, os );
        os.close();
       

        return false;
    }


};


#endif

