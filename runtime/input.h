// Cabinet controls as the JVS I/O board reports them.
#pragma once
// exit status when the window is closed; the programs themselves exit 0 to hand over
// between the game and the test menu
#define INPUT_QUIT_STATUS 64

#include <stdint.h>

// player switches, as the JVS I/O board reports them
enum {
    SW_START = 0x8000, SW_SERVICE = 0x4000, SW_UP = 0x2000, SW_DOWN = 0x1000,
    SW_LEFT = 0x0800, SW_RIGHT = 0x0400, SW_1 = 0x0200, SW_2 = 0x0100, SW_3 = 0x0080, SW_4 = 0x0040,
};
#define SYS_TEST 0x80

typedef struct {
    uint8_t system;          // bit 7 test, bit 6 tilt
    uint16_t player[2];      // JVS switch bits, first byte in the high half
    uint16_t coins[2];
    uint16_t analog[8];      // full 16-bit range, left aligned when reported
} jvs_inputs;

extern jvs_inputs jvs_in;
void input_poll(void);       // host devices and the input script into jvs_in; once per frame
