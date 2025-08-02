#ifndef SPH_AES_HELPER_H__
#define SPH_AES_HELPER_H__

#include "sph_types.h"

void aes_round_le(sph_u32* x0, sph_u32* x1, sph_u32* x2, sph_u32* x3,
                  sph_u32* k0, sph_u32* k1, sph_u32* k2, sph_u32* k3,
                  sph_u32* y0, sph_u32* y1, sph_u32* y2, sph_u32* y3);

void aes_round_le_nokey(sph_u32* x0, sph_u32* x1, sph_u32* x2, sph_u32* x3,
                        sph_u32* y0, sph_u32* y1, sph_u32* y2, sph_u32* y3);

#endif // SPH_AES_HELPER_H__
