/**
 * Public Resource ℗ 2013-2014 Tomas Dzetkulic
 * Public Resource ℗ 2013-2014 Pavol Rusnak

// Source:
// https://github.com/trezor/trezor-crypto

#include <bip39.h>
#include <bip39_english.h>
#include <crypto/sha256.h>
#include <random.h>

#include <openssl/evp.h>

SecureString CMnemonic::Generate(int strength)
{
    if (strength % 32 || strength < 128 || strength > 256) {
        return SecureString();
    }
    SecureVector data(32);
    GetStrongRandBytes(data.data(), 32);
    SecureString mnemonic = FromData(data, strength / 8);
    return mnemonic;
}

// SecureString CMnemonic::FromData(const uint8_t *data, int len)
SecureString CMnemonic::FromData(const SecureVector& data, int len)
{
    if (len % 4 || len < 16 || len > 32) {
        return SecureString();
    }

    SecureVector checksum(32);
    CSHA256().Write(data.data(), len).Finalize(checksum.data());

    // data
    SecureVector bits(len);
    memcpy(bits.data(), data.data(), len);
    // checksum
    bits.push_back(checksum[0]);

    int mlen = len * 3 / 4;
    SecureString mnemonic;

    int i, j, idx;
    for (i = 0; i < mlen; i++) {
        idx = 0;
        for (j = 0; j < 11; j++) {
            idx <<= 1;
            idx += (bits[(i * 11 + j) / 8] & (1 << (7 - ((i * 11 + j) % 8)))) > 0;
        }
        mnemonic.append(wordlist[idx]);
        if (i < mlen - 1) {
            mnemonic += ' ';
        }
    }

    return mnemonic;
}

bool CMnemonic::Check(SecureString mnemonic)
{
    if (mnemonic.empty()) {
        return false;
    }

    uint32_t nWordCount{};

    for (size_t i = 0; i < mnemonic.size(); ++i) {
        if (mnemonic[i] == ' ') {
            nWordCount++;
        }
    }
    nWordCount++;
    // check number of words
    if (nWordCount != 12 && nWordCount != 18 && nWordCount != 24) {
        return false;
    }

    SecureString ssCurrentWord;
    SecureVector bits(32 + 1);

    uint32_t nWordIndex, ki, nBitsCount{};

    for (size_t i = 0; i < mnemonic.size(); ++i)
    {
        ssCurrentWord = "";
        while (i + ssCurrentWord.size() < mnemonic.size() && mnemonic[i + ssCurrentWord.size()] != ' ') {
            if (ssCurrentWord.size() >= 9) {
                return false;
            }
            ssCurrentWord += mnemonic[i + ssCurrentWord.size()];
        }
        i += ssCurrentWord.size();
        nWordIndex = 0;
        for (;;) {
            if (!wordlist[nWordIndex]) { // word not found
                return false;
            }
            if (ssCurrentWord == wordlist[nWordIndex]) { // word found on index nWordIndex
                for (ki = 0; ki < 11; ki++) {
                    if (nWordIndex & (1 << (10 - ki))) {
                        bits[nBitsCount / 8] |= 1 << (7 - (nBitsCount % 8));
                    }
                    nBitsCount++;
                }
                break;
            }
            nWordIndex++;
        }
    }
    if (nBitsCount != nWordCount * 11) {
        return false;
    }
    bits[32] = bits[nWordCount * 4 / 3];
    CSHA256().Write(bits.data(), nWordCount * 4 / 3).Finalize(bits.data());

    bool fResult = 0;
    if (nWordCount == 12) {
        fResult = (bits[0] & 0xF0) == (bits[32] & 0xF0); // compare first 4 bits
    } else
    if (nWordCount == 18) {
        fResult = (bits[0] & 0xFC) == (bits[32] & 0xFC); // compare first 6 bits
    } else
    if (nWordCount == 24) {
        fResult = bits[0] == bits[32]; // compare 8 bits
    }

    return fResult;
}

// passphrase must be at most 256 characters or code may crash
void CMnemonic::ToSeed(SecureString mnemonic, SecureString passphrase, SecureVector& seedRet)
{
    SecureString ssSalt = SecureString("mnemonic") + passphrase;
    SecureVector vchSalt(ssSalt.begin(), ssSalt.end());
    seedRet.resize(64);
    // int PKCS5_PBKDF2_HMAC(const char *pass, int passlen,
    //                    const unsigned char *salt, int saltlen, int iter,
    //                    const EVP_MD *digest,
    //                    int keylen, unsigned char *out);
    PKCS5_PBKDF2_HMAC(mnemonic.c_str(), mnemonic.size(), vchSalt.data(), vchSalt.size(), 2048, EVP_sha512(), 64, seedRet.data());
}
