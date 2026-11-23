#ifndef _HALF_FLOAT_H_
#define _HALF_FLOAT_H_

#include <stdint.h>
#include <string.h>

// IEEE 754 binary16, for 16-bit float textures. Rounds to nearest ; values beyond its range saturate
// to the largest finite half rather than becoming infinities, which would poison blurs and mip chains.
static inline uint16_t float_to_half(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));

    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;

    if (((bits >> 23) & 0xFFu) == 0xFFu)
    {
        return (uint16_t)(sign | (mantissa ? 0x7E00u : 0x7BFFu)); // NaN stays NaN, infinities saturate
    }

    if (exponent >= 31)
    {
        return (uint16_t)(sign | 0x7BFFu);
    }

    if (exponent <= 0)
    {
        if (exponent < -10)
        {
            return (uint16_t)sign;
        }

        mantissa |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exponent);
        uint32_t half = mantissa >> shift;
        uint32_t remainder = mantissa & ((1u << shift) - 1u);

        if (remainder > (1u << (shift - 1)) || (remainder == (1u << (shift - 1)) && (half & 1u)))
        {
            half++;
        }

        return (uint16_t)(sign | half);
    }

    uint32_t half = sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
    uint32_t remainder = mantissa & 0x1FFFu;

    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u)))
    {
        half++;
    }

    return (uint16_t)half;
}

static inline float half_to_float(uint16_t value)
{
    uint32_t sign = (uint32_t)(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1Fu;
    uint32_t mantissa = value & 0x3FFu;
    uint32_t bits;

    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            bits = sign;
        }
        else
        {
            // Subnormal : normalized for the wider exponent.
            int shift = 0;

            while ((mantissa & 0x400u) == 0)
            {
                mantissa <<= 1;
                shift++;
            }

            bits = sign | ((uint32_t)(127 - 15 + 1 - shift) << 23) | ((mantissa & 0x3FFu) << 13);
        }
    }
    else if (exponent == 31)
    {
        bits = sign | 0x7F800000u | (mantissa << 13);
    }
    else
    {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }

    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

#endif // _HALF_FLOAT_H_
