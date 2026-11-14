// Key symbol to Unicode: see keys.hpp.
#include "keys.hpp"

namespace keys {

namespace {

// Cyrillic letters of keysyms 0x6C0..0x6DF (the lower case ones, in the KOI8 order of
// keysymdef.h); the upper case ones of 0x6E0..0x6FF are 0x20 lower.
const unsigned short kCyrillicLower[32] = {
    0x044E, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433, 0x0445, 0x0438, 0x0439,
    0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F, 0x044F, 0x0440, 0x0441, 0x0442, 0x0443,
    0x0436, 0x0432, 0x044C, 0x044B, 0x0437, 0x0448, 0x044D, 0x0449, 0x0447, 0x044A};

} // namespace

unsigned long KeysymToUcs(unsigned long k) {
    if ((k >= 0x20 && k <= 0x7E) || (k >= 0xA0 && k <= 0xFF))
        return k;
    if (k >= 0x01000100UL && k <= 0x0110FFFFUL)
        return k - 0x01000000UL;
    if (k >= 0x20A0 && k <= 0x20AC)
        return k;
    // Cyrillic
    if (k == 0x6A3)
        return 0x0451; // lower case yo
    if (k == 0x6B3)
        return 0x0401; // upper case yo
    if (k >= 0x6C0 && k <= 0x6DF)
        return kCyrillicLower[k - 0x6C0];
    if (k >= 0x6E0 && k <= 0x6FF)
        return static_cast<unsigned long>(kCyrillicLower[k - 0x6E0]) - 0x20;
    // Greek
    if (k >= 0x7C1 && k <= 0x7D1)
        return 0x0391 + (k - 0x7C1); // Alpha .. Rho
    if (k == 0x7D2)
        return 0x03A3; // Sigma
    if (k >= 0x7D4 && k <= 0x7D9)
        return 0x03A4 + (k - 0x7D4); // Tau .. Omega
    if (k >= 0x7E1 && k <= 0x7F1)
        return 0x03B1 + (k - 0x7E1); // alpha .. rho
    if (k == 0x7F2)
        return 0x03C3; // sigma
    if (k == 0x7F3)
        return 0x03C2; // final sigma
    if (k >= 0x7F4 && k <= 0x7F9)
        return 0x03C4 + (k - 0x7F4); // tau .. omega
    // Hebrew
    if (k >= 0xCE0 && k <= 0xCFA)
        return 0x05D0 + (k - 0xCE0);
    if (k == 0xCDF)
        return 0x2017;
    // Arabic
    if (k == 0x5AC)
        return 0x060C;
    if (k == 0x5BB)
        return 0x061B;
    if (k == 0x5BF)
        return 0x061F;
    if (k >= 0x5C1 && k <= 0x5DA)
        return 0x0621 + (k - 0x5C1);
    if (k >= 0x5E0 && k <= 0x5F2)
        return 0x0640 + (k - 0x5E0);
    return 0;
}

} // namespace keys
