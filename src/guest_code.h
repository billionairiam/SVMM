#ifndef GUEST_CODE_H
#define GUEST_CODE_H

#include <stdint.h>

/* 16-bit real mode: emit a NUL-terminated string via COM1, then halt. */
static const uint8_t guest_code[] = {
    0xba, 0xf8, 0x03,       /* mov dx, 0x3f8 */
    0x31, 0xc0,             /* xor ax, ax */
    0x8e, 0xd8,             /* mov ds, ax */
    0xbe, 0x13, 0x00,       /* mov si, 0x13 */
    0xac,                   /* lodsb */
    0x84, 0xc0,             /* test al, al */
    0x74, 0x03,             /* jz halt */
    0xee,                   /* out dx, al */
    0xeb, 0xf8,             /* jmp back to lodsb */
    0xf4,                   /* halt: hlt */
    'h', 'e', 'l', 'l', 'o', ' ', 'f', 'r',
    'o', 'm', ' ', 'g', 'u', 'e', 's', 't', '\n', '\0',
};

#endif
