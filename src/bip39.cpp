/**
 * Copyright (c) 2013-2014 Tomas Dzetkulic
 * Copyright (c) 2013-2014 Pavol Rusnak
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
 * OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// Source:
// https://github.com/trezor/trezor-crypto

#include "bip39.h"
#include "bip39_english.h"
#include "crypto/sha256.h"
#include "random.h"

#include <openssl/evp.h>

SecureString mnemonic_generate(int strength)
{
    if (strength % 32 || strength < 128 || strength > 256) {
        return SecureString();
    }
    uint8_t data[32];
    GetRandBytes(data, 32);
    SecureString mnemonic = mnemonic_from_data(data, strength / 8);
    memory_cleanse(data, sizeof(data));
    return mnemonic;
}

SecureString mnemonic_from_data(const uint8_t *data, int len)
{
    if (len % 4 || len < 16 || len > 32) {
        return SecureString();
    }

    uint8_t bits[32 + 1];

    CSHA256().Write(data, len).Finalize(bits);
    // checksum
    bits[len] = bits[0];
    // data
    memcpy(bits, data, len);

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
    memory_cleanse(bits, sizeof(bits));

    return mnemonic;
}

int mnemonic_check(SecureString mnemonic)
{
    if (mnemonic.empty()) {
        return 0;
    }

    uint32_t i{}, n{};

    while (mnemonic[i]) {
        if (mnemonic[i] == ' ') {
            n++;
        }
        i++;
    }
    n++;
    // check number of words
    if (n != 12 && n != 18 && n != 24) {
        return 0;
    }

    char current_word[10];
    uint32_t j, k, ki, bi;
    uint8_t bits[32 + 1]{};
    i = 0; bi = 0;
    while (mnemonic[i]) {
        j = 0;
        while (mnemonic[i] != ' ' && mnemonic[i] != 0) {
            if (j >= sizeof(current_word) - 1) {
                return 0;
            }
            current_word[j] = mnemonic[i];
            i++; j++;
        }
        current_word[j] = 0;
        if (mnemonic[i] != 0) i++;
        k = 0;
        for (;;) {
            if (!wordlist[k]) { // word not found
                return 0;
            }
            if (strcmp(current_word, wordlist[k]) == 0) { // word found on index k
                for (ki = 0; ki < 11; ki++) {
                    if (k & (1 << (10 - ki))) {
                        bits[bi / 8] |= 1 << (7 - (bi % 8));
                    }
                    bi++;
                }
                break;
            }
            k++;
        }
    }
    if (bi != n * 11) {
        return 0;
    }
    bits[32] = bits[n * 4 / 3];
    CSHA256().Write(bits, n * 4 / 3).Finalize(bits);

    int result = 0;
    if (n == 12) {
        result = (bits[0] & 0xF0) == (bits[32] & 0xF0); // compare first 4 bits
    } else
    if (n == 18) {
        result = (bits[0] & 0xFC) == (bits[32] & 0xFC); // compare first 6 bits
    } else
    if (n == 24) {
        result = bits[0] == bits[32]; // compare 8 bits
    }
    memory_cleanse(bits, sizeof(bits));
    return result;
}

// passphrase must be at most 256 characters or code may crash
void mnemonic_to_seed(SecureString mnemonic, SecureString passphrase, CSecureVector& seedRet)
{
    SecureString ssSalt = SecureString("mnemonic") + passphrase;
    CSecureVector vchSalt(ssSalt.begin(), ssSalt.end());
    // int PKCS5_PBKDF2_HMAC(const char *pass, int passlen,
    //                    const unsigned char *salt, int saltlen, int iter,
    //                    const EVP_MD *digest,
    //                    int keylen, unsigned char *out);
    uint8_t seed[64];
    PKCS5_PBKDF2_HMAC(mnemonic.c_str(), mnemonic.size(), &vchSalt[0], vchSalt.size(), 2048, EVP_sha512(), 64, seed);
    seedRet = CSecureVector(seed, seed + 64);
    memory_cleanse(seed, sizeof(seed));
}
