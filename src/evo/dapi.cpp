

// Copyright (c) 2014-2015 The Dash developers

// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "core_io.h"
#include "db.h"
#include "init.h"
#include "dapi.h"

int nError;
std::string strErrorMessage;
// error reporting

void ResetErrorStatus() {nError = 0; strErrorMessage = "";}
void SetError(int nErrorIn, std::string strMessageIn) {nError = nErrorIn; strErrorMessage = strMessageIn;}

/*

{ 
  "object" : "dash_budget_items",
  "data" : [
    {  "object" : "dash_budget",
    "name" : "ds_liquidity",
    ...
  },
    {  "object" : "dash_budget",
    "name" : "christmas-lottery",
    ...
  },
  ...
  ]
}
*/

Object GetResultObject(int nCommandID, std::string strCommand, Object& objFile)
{
    Object retData;
    retData.push_back(Pair("id", nCommandID));
    retData.push_back(Pair("command", strCommand));
    retData.push_back(Pair("error-id", nError));
    retData.push_back(Pair("error-message", strErrorMessage));
    retData.push_back(Pair("data", objFile));

    Object ret;
    ret.push_back(Pair("object", "dapi_result"));
    ret.push_back(Pair("data", retData));

    return ret;
}

Object GetMessageObject(int nCommandID, std::string strFromUserID, std::string strToUserID, std::string strSubCommand, std::string strMessage)
{
    Object retData;
    retData.push_back(Pair("id", nCommandID));
    retData.push_back(Pair("command", "send_message"));
    retData.push_back(Pair("error-id", nError));
    retData.push_back(Pair("error-message", strErrorMessage));
    retData.push_back(Pair("from_uid", strFromUserID));
    retData.push_back(Pair("to_uid", strToUserID));
    retData.push_back(Pair("sub_command", strSubCommand));
    retData.push_back(Pair("payload", strMessage));

    Object ret;
    ret.push_back(Pair("object", "dapi_message"));
    ret.push_back(Pair("data", retData));

    return ret;
}

std::string SerializeJsonFromObject(Object objToSerialize)
{   
    //TODO: this is terrible, we need to correctly clean and escape the json :) 
    std::stringstream ss;
    json_spirit::write( objToSerialize, ss );
    std::string strJson = escapeJsonString("'" + ss.str() + "'");
    strJson.replace(0,1,"\"");
    strJson.replace(strJson.size()-1,1,"\"");
    return strJson;
}



bool CDAPI::Execute(Object& obj)
{
    ResetErrorStatus();

    std::string strObject = json_spirit::find_value(obj, "object").get_str();
    if(strObject != "dapi_command") return false;

    Object objData = json_spirit::find_value(obj, "data").get_obj();
    string strCommand = json_spirit::find_value(objData, "command").get_str();

    printf("1 %s\n", strCommand.c_str());

    if(strCommand == "get_profile") {
        if(GetProfile(obj)) return true;
    } else if (strCommand == "set_profile") {
        if(SetProfile(obj)) return true;
    } else if (strCommand == "set_private_data") {
        if(SetPrivateData(obj)) return true;
    } else if (strCommand == "get_private_data") {
        if(GetPrivateData(obj)) return true;
    } else if (strCommand == "send_message") {
        if(SendMessage(obj)) return true;
    } else if (strCommand == "broadcast_message") {
        if(BroadcastMessage(obj)) return true;
    }

    // unknown command

    // send the user back the results of the query
    if(nError == 0) SetError(1000, "Unknown Command : " + strCommand);
    Object result;
    Object ret = GetResultObject(1000, strCommand, result);
    std::string strJson = SerializeJsonFromObject(ret);

    EventNotify(strJson);

    return false;
}

bool CDAPI::ValidateSignature(Object& obj)
{
    /*
        lookup pubkey for user
        remove signature
        hash object
        check signature against hash
    */

    return true;
}

bool CDAPI::GetProfile(Object& obj)
{
    /*
        {   
            "object" : "dapi_command",
            "data" : {
                “command” : ”get_profile”,
                “from_uid” : INT64,
                “to_uid” : INT64, 
                “signature” : ‘’,
                “fields” : [“fname”, “lname”]
            }
        }
    */

    std::string strObject = json_spirit::find_value(obj, "object").get_str();

    printf("3 %s\n", strObject.c_str());

    // get the user we want to open
    Object objData = json_spirit::find_value(obj, "data").get_obj();
    string strUID = json_spirit::find_value(objData, "to_uid").get_str();

    printf("4 %s\n", strUID.c_str());

    // open the file and read it
    CDriveFile file(GetProfileFile(strUID));
    printf("5 %s\n", GetProfileFile(strUID).c_str());

    if(!file.Exists()) 
    {
        SetError(1001, "File doesn't exist : " + strUID);
        return false;
    }
    file.Read();

    printf("6 %s\n", GetProfileFile(strUID).c_str());

    // send the user back the results of the query
    Object ret = GetResultObject(1000, "get_profile", file.obj);

    std::string strJson2 = SerializeJsonFromObject(file.obj);

    printf("7 %s\n", strJson2.c_str());
    
    std::string strJson = SerializeJsonFromObject(ret);

    printf("8 %s\n", strJson.c_str());

    EventNotify(strJson);
    return true;
}

bool CDAPI::SetProfile(Object& obj)
{
    /*
    { 
        "object" : "dapi_command",
        "data" : {
            "command": "set_profile",
            "from_uid": INT64,
            "to_uid": INT64, 
            "signature": "",
            "update" : [
                {"field":"name","value":"newvalue"}
            ]
        }
    }

    */

    std::string strObject = json_spirit::find_value(obj, "object").get_str();

    printf("2 %s\n", strObject.c_str());
    // get the user we want to open
    Object objData = json_spirit::find_value(obj, "data").get_obj();
    const Array& arrDataUpdate = json_spirit::find_value(objData, "update").get_array();
    string strUID = json_spirit::find_value(objData, "to_uid").get_str();

    printf("3 %s\n", strUID.c_str());

    // open the file and read it
    CDriveFile file(GetProfileFile(strUID));
    if(!file.Exists())
    {
        SetError(1002, "File doesn't exist : " + strUID);
        return false;
    }
    file.Read();

    std::map<std::string, Value> mapObj;
    json_spirit::obj_to_map(file.obj, mapObj);

    printf("4 %s\n", GetProfileFile(strUID).c_str());

    for( unsigned int i = 0; i < arrDataUpdate.size(); ++i )
    {
        Object tmp = arrDataUpdate[i].get_obj();
        string strField = json_spirit::find_value(tmp, "field").get_str();
        string strValue = json_spirit::find_value(tmp, "value").get_str();

        printf("5 %s %s\n", strField.c_str(), strValue.c_str());

        // string& strUpdate = json_spirit::find_value(obj, strField).get_str();
        // strUpdate = strValue;

        //update the users file, NOTE: this is completely insecure for the prototype (see the paper for security model!)
        //file.obj[strField] = strValue;
        mapObj[strField] = strValue;
    }

    json_spirit::map_to_obj(mapObj, file.obj);
    file.Write();

    Object ret = GetResultObject(1000, "set_profile", file.obj);
    std::string strJson = SerializeJsonFromObject(ret);

    printf("7 %s\n", strJson.c_str());

    EventNotify(strJson);

    return true;
}

bool CDAPI::GetPrivateData(Object& obj)
{
    /*
    {
        "object" : "dapi_command",
        "data" : {
            "command" = "get_private_data",
            "from_uid" = UID,
            "to_uid" = UID, 
            "signature" = ‘’,
            "slot" = 1
        }
    }
    */

    std::string strObject = json_spirit::find_value(obj, "object").get_str();

    printf("3 %s\n", strObject.c_str());

    // get the user we want to open
    Object objData = json_spirit::find_value(obj, "data").get_obj();
    string strUID = json_spirit::find_value(objData, "to_uid").get_str();
    int nSlot = json_spirit::find_value(objData, "slot").get_int();
    if(nSlot < 1 || nSlot > 10)
    {
        SetError(1002, "Slot out of range");
        return false;
    }

    printf("4 %s\n", strUID.c_str());

    // open the file and read it
    CDriveFile file(GetPrivateDataFile(strUID, nSlot));
    printf("5 %s\n", GetPrivateDataFile(strUID, nSlot).c_str());

    if(!file.Exists()) return false;
    file.Read();

    // send the user back the results of the query
    Object ret = GetResultObject(1000, "get_private_data", file.obj);
    std::string strJson = SerializeJsonFromObject(ret);

    printf("7 %s\n", strJson.c_str());

    EventNotify(strJson);

    return true;
}

bool CDAPI::SetPrivateData(Object& obj)
{
    /*
    {
        "object" : "dapi_command",
        "data" : {
            "command" : "set_private_data",
            "from_uid" : INT64,
            "to_uid" : INT64, 
            "signature" : "SIGNATURE",
            "slot" : 1,
            "payload" : JSON_WEB_ENCRYPTION
        }
    }
    */

    std::string strObject = json_spirit::find_value(obj, "object").get_str();

    printf("3 %s\n", strObject.c_str());

    // get the user we want to open
    Object objData = json_spirit::find_value(obj, "data").get_obj();
    string strUID = json_spirit::find_value(objData, "to_uid").get_str();
    int nSlot = json_spirit::find_value(objData, "slot").get_int();
    if(nSlot < 1 || nSlot > 10) 
    {
        SetError(1002, "Slot out of range");
        return false;
    }

    printf("4 %s\n", strUID.c_str());

    // open the file and read it
    CDriveFile file(GetPrivateDataFile(strUID, nSlot));
    printf("5 %s\n", GetPrivateDataFile(strUID, nSlot).c_str());

    if(!file.Exists())
    {
        Object newObj;
        newObj.push_back(Pair("access_times", 0));
        newObj.push_back(Pair("last_access", 0));
        newObj.push_back(Pair("payload", ""));
        file.obj = newObj;   
    }
    file.Read();

    //update from new payload
    std::map<std::string, Value> mapObj;
    json_spirit::obj_to_map(file.obj, mapObj);

    string strPayload = json_spirit::find_value(objData, "payload").get_str();
    mapObj["payload"] = strPayload;

    json_spirit::map_to_obj(mapObj, file.obj);
    file.Write();


    printf("6 %s\n", GetPrivateDataFile(strUID, nSlot).c_str());

    // send the user back the results of the query
    Object ret = GetResultObject(1000, "set_private_data", file.obj);
    std::string strJson = SerializeJsonFromObject(ret);

    printf("7 %s\n", strJson.c_str());

    EventNotify(strJson);

    return true;
    
}

// send message from one user to another through T2
bool CDAPI::SendMessage(Object& obj)
{
    /*
    { 
        "object" : "dapi_command",
        "data" : {
            "command" = "send_message",
            "sub_command" = "(addr,cmd2,cmd3)",
            "from_uid" = UID,
            "to_uid" = UID, 
            "signature" = ‘’,
            "payload" = ENCRYPTED
        }
    }
    */

    std::string strObject = json_spirit::find_value(obj, "object").get_str();

    printf("3 %s\n", strObject.c_str());

    // get the user we want to open
    Object objData = json_spirit::find_value(obj, "data").get_obj();
    string strUID1 = json_spirit::find_value(objData, "from_uid").get_str();
    string strUID2 = json_spirit::find_value(objData, "to_uid").get_str();
    string strSubCommand = json_spirit::find_value(objData, "sub_command").get_str();
    string strPayload = json_spirit::find_value(objData, "payload").get_str();

    //TODO: this is presently sending the message to all users on the server
    Object ret = GetMessageObject(1000, strUID1, strUID2, strSubCommand, strPayload);
    std::string strJson = SerializeJsonFromObject(ret);

    printf("4 %s\n", strJson.c_str());
    EventNotify(strJson);

    return true;
}

// broadcast any message on the network from T3
bool CDAPI::BroadcastMessage(Object& obj)
{
    /*
    { 
        "object" : "dapi_command",
        "data" : {
            "command" = "broadcast",
            "sub_command" = "tx", //can support multiple message commands
            "from_uid" = UID,
            "to_uid" = UID, 
            "signature" = ‘’,
            "payload" = SERIALIZED_BASE64_ENCODED
        }
    }
    */


    std::string strObject = json_spirit::find_value(obj, "object").get_str();

    printf("3 %s\n", strObject.c_str());

    // get the user we want to open
    Object objData = json_spirit::find_value(obj, "data").get_obj();
    string strSubCommand = json_spirit::find_value(objData, "sub_command").get_str();
    string strPayload = json_spirit::find_value(objData, "payload").get_str();

    if(strSubCommand == "tx")
    {
        CTransaction tx;
        if (!DecodeHexTx(tx, strPayload))
        {
            SetError(1003, "TX Decoding Failed");
            return false;
        }
        RelayTransaction(tx);

        Object retTx;
        retTx.push_back(Pair("tx-id", tx.GetHash().ToString()));
        //should probably figure out if it was broadcasted successfully

        // send the user back the results of the query
        Object ret = GetResultObject(1000, "broadcast_message", retTx);
        std::string strJson = SerializeJsonFromObject(ret);

    } else {
        return false;
    }

    return false;
}

// Create new user account
bool CDAPI::InviteUser(Object& obj)
{
    // //SEND FRIEND REQUEST
    // {
    //     "object" : "dapi_command",
    //     "data" : {
    //         "command" : “invite_user”,
    //         "from_uid" : UID,
    //         "to_uid" : ENTERED_USERNAME,
    //         "to_email" : ENTERED_EMAIL, 
    //         "to_pubkey" : ENTERED_PUBKEY, 
    //         "signature" = “”
    //     }
    // }

    std::string strObject = json_spirit::find_value(obj, "object").get_str();

    printf("3 %s\n", strObject.c_str());

    // get the user we want to open
    Object objData = json_spirit::find_value(obj, "data").get_obj();
    string strUID = json_spirit::find_value(objData, "to_uid").get_str();

    printf("4 %s\n", strUID.c_str());

    // open the file and read it
    CDriveFile file(GetProfileFile(strUID));
    printf("5 %s\n", GetProfileFile(strUID).c_str());

    if(file.Exists()) 
    {
        SetError(1004, "User already exists : " + strUID);
        return false;
    }

    string strName = json_spirit::find_value(objData, "to_name").get_str();
    string strEmail = json_spirit::find_value(objData, "to_email").get_str();
    string strPubkey = json_spirit::find_value(objData, "to_pubkey").get_str();

    CBitcoinAddress address(strPubkey);
    bool isValid = address.IsValid();

    if(!isValid)
    {
        SetError(1005, "Invalid pubkey: " + strPubkey);
        return false;   
    }

    Object newObj;
    newObj.push_back(Pair("status", 1));
    newObj.push_back(Pair("name", strName));
    newObj.push_back(Pair("email", strEmail));
    newObj.push_back(Pair("pubkey", strPubkey));
    newObj.push_back(Pair("challenge_code", GetRand(999999)));
    file.obj = newObj;   

    file.Write();

    // //RESULTING JSON
    // {
    //     "object" : "dapi_result",
    //     "data" : {
    //         "command" : “invite_user”,
    //         "from_uid" : UID,
    //         "to_uid" : ENTERED_USERNAME,
    //         "to_email" : ENTERED_EMAIL, 
    //         "to_pubkey" : ENTERED_PUBKEY, 
    //         "to_challenge_code" : RANDOMLY_GENERATED, 
    //         "signature" : “”
    //     }
    // }

    // send the user back the results of the query
    Object ret = GetResultObject(1000, "invite_user", file.obj);
    std::string strJson = SerializeJsonFromObject(ret);

    printf("7 %s\n", strJson.c_str());

    EventNotify(strJson);
}

bool CDAPI::ValidateAccount(Object& obj)
{
    /*
        //VALIDATE ACCOUNT
        {
            "object" : "dapi_command",
            "data" : {
                "command" : “validate_account”,
                "from_uid" : UID,
                "to_uid" : ENTERED_USERNAME,
                "to_challenge_code" : RANDOMLY_GENERATED, 
                "signature" : “”
            }
        }
    */

    std::string strObject = json_spirit::find_value(obj, "object").get_str();

    printf("3 %s\n", strObject.c_str());

    // get the user we want to open
    Object objData = json_spirit::find_value(obj, "data").get_obj();
    string strUID = json_spirit::find_value(objData, "to_uid").get_str();
    string strChallengeCode = json_spirit::find_value(objData, "to_challenge_code").get_str();

    printf("4 %s\n", strUID.c_str());

    // open the file and read it
    CDriveFile file(GetProfileFile(strUID));
    printf("5 %s\n", GetProfileFile(strUID).c_str());

    // open the file and read it
    CDriveFile file(GetProfileFile(strUID));
    printf("5 %s\n", GetProfileFile(strUID).c_str());

    if(!file.Exists()) 
    {
        SetError(1001, "File doesn't exist : " + strUID);
        return false;
    }
    file.Read();

    std::map<std::string, Value> mapObj;
    json_spirit::obj_to_map(file.obj, mapObj);

    string strUID = json_spirit::find_value(objData, "to_uid").get_str();
    if(mapObj["challenge_code"] == strChallengeCode)
    {
        mapObj["challenge_code"] = "";
        mapObj["status"] = 2;
    } else {
        SetError(1006, "Invalid challenge_code : " + strChallengeCode);
        return false;
    }
    
    json_spirit::map_to_obj(mapObj, file.obj);
    file.Write();

    /*
        //RESULTING JSON
        {
            "object" : "dapi_result",
            "data" : {
                "command" : “validate_account”,
                "from_uid" : UID,
                "to_uid" : ENTERED_USERNAME, 
                "signature" : “”,
                “error_id” : 1000,
                “error_description” : “none” 
            }
        }
    */

    // send the user back the results of the query
    Object ret = GetResultObject(1000, "“validate_account”", file.obj);
    std::string strJson = SerializeJsonFromObject(ret);

    printf("7 %s\n", strJson.c_str());

    EventNotify(strJson);
}

