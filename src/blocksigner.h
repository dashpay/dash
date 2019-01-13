#ifndef BLOCKSIGNER_H
#define BLOCKSIGNER_H

class CBlock;
class CPubKey;
class CKey;
class CKeyStore;

struct CBlockSigner {

    CBlockSigner(CBlock &block, const CKeyStore *keystore);

    bool SignBlock();
    bool CheckBlockSignature() const;

    CBlock &refBlock;
    const CKeyStore *refKeystore;
};
#endif // BLOCKSIGNER_H