#ifndef SPH_AES_HELPER_H__
#define SPH_AES_HELPER_H__

#include <cstdint>

#include <array>
#include <cstdint>

void aes_round_le(uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3,
                  uint32_t k0, uint32_t k1, uint32_t k2, uint32_t k3,
                  uint32_t& y0, uint32_t& y1, uint32_t& y2, uint32_t& y3);

void aes_round_le_nokey(uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3,
                        uint32_t& y0, uint32_t& y1, uint32_t& y2, uint32_t& y3);

void SapphireAutoDetect();

#endif // SPH_AES_HELPER_H__
