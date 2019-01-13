#include "blocksigner.h"
#include "keystore.h"
#include "primitives/block.h"
#include "utilstrencodings.h"
#include "util.h"
CBlockSigner::CBlockSigner(CBlock &block, const CKeyStore *keystore) :
        refBlock(block),
        refKeystore(keystore)
{

}
 bool CBlockSigner::SignBlock()
{
    std::vector<std::vector<unsigned char>> vSolutions;
    txnouttype whichType;
     CKey keySecret;
    if(refBlock.IsProofOfStake())
    {
        const CTxOut& txout = refBlock.vtx[1]->vout[1];
        if (!Solver(txout.scriptPubKey, whichType, vSolutions))
            return false;
        else
        {
            CKeyID keyID;
             if (whichType == TX_PUBKEYHASH)
            {
                keyID = CKeyID(uint160(vSolutions[0]));
            }
            else if(whichType == TX_PUBKEY)
            {
                keyID = CPubKey(vSolutions[0]).GetID();
            }
             if (!refKeystore->GetKey(keyID, keySecret))
                return false;
        }
    }
    else
    {
        const CTxOut& txout = refBlock.vtx[0]->vout[0];
         if (!Solver(txout.scriptPubKey, whichType, vSolutions))
            return false;
         CKeyID keyID;
         if (whichType == TX_PUBKEYHASH)
        {
            keyID = CKeyID(uint160(vSolutions[0]));
        }
        else if(whichType == TX_PUBKEY)
        {
            keyID = CPubKey(vSolutions[0]).GetID();
        }
         if (!refKeystore->GetKey(keyID, keySecret))
            return false;
    }
     return keySecret.SignCompact(refBlock.GetHash(), refBlock.vchBlockSig);
}
 bool CBlockSigner::CheckBlockSignature() const
{
    if(refBlock.IsProofOfWork())
        return true;
     if(refBlock.vchBlockSig.empty())
        return false;
     std::vector<std::vector<unsigned char>> vSolutions;
    txnouttype whichType;
     const CTxOut& txout = refBlock.vtx[1]->vout[1];
     if (!Solver(txout.scriptPubKey, whichType, vSolutions))
        return false;
     CKeyID signatureKeyID;
    CPubKey recoveredKey;
     auto hashMessage = refBlock.GetHash();
     if(!recoveredKey.RecoverCompact(hashMessage, refBlock.vchBlockSig))
        return ("CBlockSigner::CheckBlockSignature() : failed to recover public key from signature");
     if(refBlock.IsProofOfStake())
    {
            if (whichType == TX_PUBKEYHASH)
            {
                signatureKeyID = CKeyID(uint160(vSolutions[0]));
            }
            else if(whichType == TX_PUBKEY)
            {
                signatureKeyID = CPubKey(vSolutions[0]).GetID();
            }
    }
    else
    {
        const CTxOut& txout = refBlock.vtx[0]->vout[0];
         if (!Solver(txout.scriptPubKey, whichType, vSolutions))
            return false;
         if (whichType == TX_PUBKEYHASH)
        {
            signatureKeyID = CKeyID(uint160(vSolutions[0]));
        }
        else if(whichType == TX_PUBKEY)
        {
            signatureKeyID = CPubKey(vSolutions[0]).GetID();
        }
    }
     return recoveredKey.GetID() == signatureKeyID;
}
