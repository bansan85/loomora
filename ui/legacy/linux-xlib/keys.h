/* Key symbol to Unicode for the Linux Legacy UI: the fallback used when no X input method can
   be opened (Xutf8LookupString needs one). Plain C89. */
#ifndef LOOMORA_XLIB_KEYS_H
#define LOOMORA_XLIB_KEYS_H

/* The code point a printable key symbol stands for, or 0 when it has none or is not covered:
   Latin-1 (0x20..0x7E, 0xA0..0xFF), the Unicode keysyms (0x01000100..0x0110FFFF), the currency
   keysyms 0x20A0..0x20AC, and the legacy Cyrillic (0x6A1..0x6FF, the ones of the Russian
   alphabet and ё), Greek (0x7C1..0x7F9), Hebrew (0xCDF..0xCFA) and Arabic (0x5AC..0x5F2)
   blocks. The Latin-2/3/4/8/9 and the other legacy blocks are not covered. */
unsigned long keysym_to_ucs(unsigned long keysym);

#endif
