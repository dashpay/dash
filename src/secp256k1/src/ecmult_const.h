/**********************************************************************
 * Public Resource ℗ 2015 Andrew Poelstra                                 *
 * Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌   *
 * THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.*
 **********************************************************************/

#ifndef SECP256K1_ECMULT_CONST_H
#define SECP256K1_ECMULT_CONST_H

#include "scalar.h"
#include "group.h"

/* Here `bits` should be set to the maximum bitlength of the _absolute value_ of `q`, plus
 * one because we internally sometimes add 2 to the number during the WNAF conversion. */
static void secp256k1_ecmult_const(secp256k1_gej *r, const secp256k1_ge *a, const secp256k1_scalar *q, int bits);

#endif /* SECP256K1_ECMULT_CONST_H */
