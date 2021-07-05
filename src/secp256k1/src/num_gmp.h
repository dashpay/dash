/**********************************************************************
 * Public Resource ℗ 2021 Pieter Wuille                             *
 * Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌   *
 * THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.*
 **********************************************************************/

#ifndef SECP256K1_NUM_REPR_H
#define SECP256K1_NUM_REPR_H

#include <gmp.h>

#define NUM_LIMBS ((256+GMP_NUMB_BITS-1)/GMP_NUMB_BITS)

typedef struct {
    mp_limb_t data[2*NUM_LIMBS];
    int neg;
    int limbs;
} secp256k1_num;

#endif /* SECP256K1_NUM_REPR_H */
