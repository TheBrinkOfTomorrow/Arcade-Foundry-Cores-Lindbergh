// The Lindbergh cabinet: baseboard (shared memory, SRAM, JVS bus), EEPROM,
// security and DIP-switch functions and libkswapapi. Audio is in hle_audio.c.
// Written from the public JVS protocol and observed driver behaviour.
#include "af_debug.h"
#include "hle.h"
#include "host.h"
#include "input.h"
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>

static char save_dir[1100];

static FILE *open_save(const char *name, size_t size)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", save_dir, name);
    FILE *f = fopen(p, "rb+");
    if (!f) {
        f = fopen(p, "wb+");
        if (!f) {
            char why[1300];
            snprintf(why, sizeof why, "cannot create %s", p);
            host_fatal(why);
        }
        for (size_t i = 0; i < size; i++) fputc(0, f);
        fflush(f);
    }
    return f;
}

// ---- JVS I/O board ---------------------------------------------------------------
// One SEGA type 3 board: 2 players, 14 switches each, 2 coin slots, 8 analog
// channels of 10 bits, 20 general purpose outputs.
static const char jvs_id[] = "SEGA CORPORATION;I/O BD JVS;837-14572;Ver1.00;2005/10";
static int jvs_addr = -1;
static int sense = 3;          // 3: no address yet, 1: addressed
static uint8_t jvs_last[1024];
static int jvs_last_n;
static pthread_mutex_t jvs_lock = PTHREAD_MUTEX_INITIALIZER;

// decode one escaped packet: returns payload length (dest, len, data..., sum) or -1
static int jvs_unescape(const uint8_t *in, int n, uint8_t *out)
{
    int i = 0, o = 0;
    while (i < n && in[i] != 0xe0) i++;
    if (i >= n) return -1;
    i++;
    while (i < n && o < 1024) {
        uint8_t b = in[i++];
        if (b == 0xd0 && i < n) b = in[i++] + 1;
        out[o++] = b;
        if (o >= 2 && o == out[1] + 2) break;
    }
    return o;
}

static int jvs_escape(const uint8_t *in, int n, uint8_t *out)
{
    int o = 0;
    out[o++] = 0xe0;
    for (int i = 0; i < n; i++) {
        if (in[i] == 0xe0 || in[i] == 0xd0) {
            out[o++] = 0xd0;
            out[o++] = in[i] - 1;
        } else out[o++] = in[i];
    }
    return o;
}

// process one request packet, write the escaped reply to out; returns its size
static int jvs_process(const uint8_t *in, int n, uint8_t *out)
{
    uint8_t p[1024], r[1024];
    int len = jvs_unescape(in, n, p);
    if (len < 3) return 0;
    uint8_t dest = p[0];
    if (dest != 0xff && dest != jvs_addr) return 0;
    const uint8_t *d = p + 2;
    int dn = p[1] - 1;       // command bytes, without the checksum
    int o = 3;               // r[0] dest, r[1] len, r[2] status
    r[0] = 0x00;
    r[2] = 0x01;
    pthread_mutex_lock(&jvs_lock);
    for (int i = 0; i < dn;) {
        switch (d[i]) {
        case 0xf0:           // reset
            jvs_addr = -1;
            sense = 3;
            i += 2;
            break;
        case 0xf1:           // assign address
            jvs_addr = d[i + 1];
            sense = 1;
            r[o++] = 1;
            i += 2;
            break;
        case 0x10:           // identify
            r[o++] = 1;
            memcpy(r + o, jvs_id, sizeof jvs_id);
            o += sizeof jvs_id;
            i += 1;
            break;
        case 0x11: r[o++] = 1; r[o++] = 0x13; i += 1; break;   // command format revision
        case 0x12: r[o++] = 1; r[o++] = 0x30; i += 1; break;   // JVS revision
        case 0x13: r[o++] = 1; r[o++] = 0x10; i += 1; break;   // communication version
        case 0x14: {         // feature check
            static const uint8_t feat[] = {
                0x01, 2, 14, 0,      // switches: players, switches per player
                0x02, 2, 0, 0,       // coin slots
                0x03, 8, 10, 0,      // analog: channels, bits
                0x12, 20, 0, 0,      // general purpose outputs
                0x00,
            };
            r[o++] = 1;
            memcpy(r + o, feat, sizeof feat);
            o += sizeof feat;
            i += 1;
            break;
        }
        case 0x15:           // main board id: a string
            r[o++] = 1;
            i++;
            while (i < dn && d[i]) i++;
            i++;
            break;
        case 0x20: {         // switch inputs: players, bytes per player
            int pl = d[i + 1], bytes = d[i + 2];
            r[o++] = 1;
            r[o++] = jvs_in.system;
            for (int k = 0; k < pl; k++)
                for (int b = 0; b < bytes; b++)
                    r[o++] = k < 2 && b < 2 ? (uint8_t)(jvs_in.player[k] >> (8 - 8 * b)) : 0;
            i += 3;
            break;
        }
        case 0x21: {         // coin counters
            int slots = d[i + 1];
            r[o++] = 1;
            for (int k = 0; k < slots; k++) {
                uint16_t v = k < 2 ? jvs_in.coins[k] & 0x3fff : 0;
                r[o++] = (uint8_t)(v >> 8);
                r[o++] = (uint8_t)v;
            }
            i += 2;
            break;
        }
        case 0x22: {         // analog inputs, left aligned 10 bits
            int ch = d[i + 1];
            r[o++] = 1;
            for (int k = 0; k < ch; k++) {
                uint16_t v = k < 8 ? jvs_in.analog[k] & 0xffc0 : 0;
                r[o++] = (uint8_t)(v >> 8);
                r[o++] = (uint8_t)v;
            }
            i += 2;
            break;
        }
        case 0x2f:           // retransmit the last reply
            pthread_mutex_unlock(&jvs_lock);
            memcpy(out, jvs_last, jvs_last_n);
            return jvs_last_n;
        case 0x30: {         // coin decrease: slot, amount
            int s = d[i + 1] - 1;
            uint16_t amt = (uint16_t)(d[i + 2] << 8 | d[i + 3]);
            if (s >= 0 && s < 2) jvs_in.coins[s] = jvs_in.coins[s] > amt ? jvs_in.coins[s] - amt : 0;
            r[o++] = 1;
            i += 4;
            break;
        }
        case 0x31: {         // coin increase
            int s = d[i + 1] - 1;
            if (s >= 0 && s < 2) jvs_in.coins[s] += (uint16_t)(d[i + 2] << 8 | d[i + 3]);
            r[o++] = 1;
            i += 4;
            break;
        }
        case 0x32:           // general purpose output: byte count, bytes
            r[o++] = 1;
            i += 2 + d[i + 1];
            break;
        default:
            hle_log("jvs: unknown command 0x%02x\n", d[i]);
            r[2] = 0x02;
            i = dn;
            break;
        }
    }
    pthread_mutex_unlock(&jvs_lock);
    r[1] = (uint8_t)(o - 2 + 1);
    uint8_t sum = 0;
    for (int k = 0; k < o; k++) sum += r[k];
    r[o++] = sum;
    int m = jvs_escape(r, o, out);
    memcpy(jvs_last, out, m);
    jvs_last_n = m;
    return m;
}

// ---- baseboard: /dev/lbb ------------------------------------------------------------
static uint8_t shm[64 * 1024];     // the game addresses past 32 KB (serial reply at 0x9800+96)
static uint32_t shm_pos;
static FILE *sram;
static int lbb_ready = -1;
static uint32_t jvs_dest, jvs_size, serial_dest;
static const char serial[] = "FE11-X018012022X";

static int lbb_open(void)
{
    if (!sram) sram = open_save("sram.bin", 64 * 1024);
    return 0;
}

static int lbb_ioctl(uint32_t req, uint32_t a)
{
    switch (req) {
    case 0x300: return 0;                           // init
    case 0x201: lbb_ready = 0; return 0;            // data ready: select reports 0
    case 0x8004bc02:                                // firmware version
        wr8(a, 0x00); wr8(a + 1, 0x19); wr8(a + 2, 0x20); wr8(a + 3, 0x07);
        return 0;
    case 0x400: shm_pos = a % sizeof shm; return 0;  // seek in shared memory
    case 0x601:                                     // read SRAM {data, offset, size}
        fseek(sram, rd32(a + 4), SEEK_SET);
        if (fread(G(rd32(a)), 1, rd32(a + 8), sram)) {}
        return 0;
    case 0x600:                                     // write SRAM {offset, data, size}
        fseek(sram, rd32(a), SEEK_SET);
        fwrite(G(rd32(a + 4)), 1, rd32(a + 8), sram);
        fflush(sram);
        return 0;
    case 0xc020bc06: {                              // request
        uint32_t cmd = rd32(a);
        switch (cmd) {
        case 0x120: serial_dest = rd32(a + 4); break;
        case 0x180: hle_log("baseboard: flash write ignored\n"); break;
        case 0x210: break;
        case 0x220: {
            uint32_t src = rd32(a + 4), n = rd32(a + 8);
            jvs_dest = rd32(a + 12);
            if (src + n > sizeof shm) n = 0;
            uint8_t out[2048];
            int m = jvs_process(shm + src, (int)n, out);
            if (jvs_dest + m > sizeof shm) m = 0;
            memcpy(shm + jvs_dest, out, m);
            jvs_size = m;
            break;
        }
        default: hle_log("baseboard: unknown request 0x%x\n", cmd);
        }
        wr32(a, cmd | 0xf0000000u);
        return 0;
    }
    case 0xc020bc07: {                              // receive
        uint32_t cmd = rd32(a) & 0xfff;
        switch (cmd) {
        case 0x120:
            if (serial_dest + 96 + sizeof serial < sizeof shm)
                memcpy(shm + serial_dest + 96, serial, sizeof serial - 1);
            wr32(a + 4, 1);
            break;
        case 0x210:
            wr32(a + 8, sense);
            wr32(a + 4, 1);
            break;
        case 0x220:
            wr32(a + 8, jvs_dest);
            wr32(a + 12, jvs_size);
            wr32(a + 4, 1);
            break;
        default: hle_log("baseboard: unknown receive 0x%x\n", cmd);
        }
        wr32(a, rd32(a) | 0xf0000000u);
        return 0;
    }
    }
    hle_log("baseboard: unknown ioctl 0x%x\n", req);
    return 0;
}

static int lbb_read(uint32_t buf, uint32_t n)
{
    if (shm_pos + n > sizeof shm) n = sizeof shm - shm_pos;
    memcpy(G(buf), shm + shm_pos, n);
    return (int)n;
}

static int lbb_write(uint32_t buf, uint32_t n)
{
    if (shm_pos + n > sizeof shm) n = sizeof shm - shm_pos;
    memcpy(shm + shm_pos, G(buf), n);
    return (int)n;
}

static int lbb_select(void) { return lbb_ready; }

static const device_ops lbb_dev = {"/dev/lbb", lbb_open, lbb_ioctl, lbb_read, lbb_write, lbb_select};

// ---- EEPROM on the SMBus: /dev/i2c/0 --------------------------------------------------
static FILE *eeprom;

#ifdef LB_REGION
// amLib's static system data opens the EEPROM (amSysDataRecord_Static: 32
// bytes at 0, a copy at 0x200), its CRC first: byte 14 is the cabinet's
// region, 0 Japan, 1 USA, 2 Export, which the games read through amLib (HotD4's
// CConfigMgr::GetRegion, OutRun 2 SP's asgGetCountryCode) and their test menus
// show. A new EEPROM has none and reads as Japan: the runtime writes the region
// the front end chose (lb_region) whenever the game opens it
extern int lb_region;

// amCrc32RGet: reflected CRC-32 from ~0, not inverted at the end
static uint32_t am_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffff;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = crc >> 1 ^ (crc & 1 ? 0xedb88320 : 0);
    }
    return crc;
}

static void eeprom_set_region(void)
{
    static const uint8_t region[3] = {2, 1, 0};
    for (long at = 0; at <= 0x200; at += 0x200) {
        uint8_t r[32];
        fseek(eeprom, at, SEEK_SET);
        if (fread(r, 1, sizeof r, eeprom) != sizeof r) continue;
        uint32_t crc = r[0] | r[1] << 8 | r[2] << 16 | (uint32_t)r[3] << 24;
        if (crc != am_crc32(r + 4, 28)) {   // none yet: amSysData_InitStatic's
            memset(r, 0, sizeof r);
            memcpy(r + 15, "AAGX-01A00009999", 16);
        } else if (r[14] == region[lb_region]) continue;
        r[14] = region[lb_region];
        crc = am_crc32(r + 4, 28);
        for (int i = 0; i < 4; i++) r[i] = (uint8_t)(crc >> 8 * i);
        fseek(eeprom, at, SEEK_SET);
        fwrite(r, 1, sizeof r, eeprom);
    }
    fflush(eeprom);
    fseek(eeprom, 0, SEEK_SET);
}
#endif

static int i2c_open(void)
{
    if (!eeprom) eeprom = open_save("eeprom.bin", 8192);
#ifdef LB_REGION
    if (eeprom) eeprom_set_region();
#endif
    return 0;
}

static int i2c_ioctl(uint32_t req, uint32_t a)
{
    switch (req) {
    case 0x705: wr32(a, 0x0fff0009); return 0;     // I2C_FUNCS: plain I2C plus every SMBus transfer
    case 0x703: case 0x1261: case 0xc04064a0: return 0;
    case 0x720: {    // SMBus transfer {u8 read_write, u8 command, u32 size, data*}
        uint8_t cmdb = rd8(a + 1);
        uint32_t size = rd32(a + 4), data = rd32(a + 8);
        switch (size) {
        case 1: {    // read the byte at the current address
            uint8_t v = 0;
            if (fread(&v, 1, 1, eeprom)) {}
            wr8(data, v);
            break;
        }
        case 2:      // set the address
            fseek(eeprom, cmdb << 8 | rd8(data), SEEK_SET);
            break;
        case 3: {    // write one byte
            uint8_t v = (uint8_t)(rd16(data) >> 8);
            fseek(eeprom, cmdb << 8 | rd8(data), SEEK_SET);
            fwrite(&v, 1, 1, eeprom);
            fflush(eeprom);
            break;
        }
        case 6: {    // I2C block (Let's Go Jungle's M24Cxx code): data[0] = count
            uint32_t n = rd8(data);
            if (n > 32) n = 32;
            if (rd8(a) == 1) {           // read from the current address
                if (fread(G(data + 1), 1, n, eeprom) != n) memset(G(data + 1), 0xff, n);
            } else if (n) {              // page write: data[1] = low address byte, then the bytes
                fseek(eeprom, cmdb << 8 | rd8(data + 1), SEEK_SET);
                fwrite(G(data + 2), 1, n - 1, eeprom);
                fflush(eeprom);
            }
            break;
        }
        default: hle_log("eeprom: unknown transfer size %d\n", size);
        }
        return 0;
    }
    }
    hle_log("eeprom: unknown ioctl 0x%x\n", req);
    return 0;
}

static const device_ops i2c_dev = {"/dev/i2c/0", i2c_open, i2c_ioctl, NULL, NULL, NULL};

// ---- steering drive unit on the serial port: /dev/tts/0 ---------------------------------
// The force-feedback motor. Packets are 4 bytes: command | 0x80, two 7-bit data bytes
// and an XOR check; steering itself comes over JVS. Race TV only reads back the wheel
// position (reply 0x14, a centred one here). OutRun 2 SP's board answers each packet with
// a status byte, the status in both nibbles: 1 after command 0x01 (the motor check its
// start-up runs), 0 (ready) otherwise; it homes the wheel on the JVS steering input.
static uint8_t drive_out[256];
static int drive_n, drive_pos;
static uint8_t drive_cmd[4];
static int drive_cmd_n;

static int drive_open(void) { drive_n = drive_pos = drive_cmd_n = 0; return 0; }

static int drive_ioctl(uint32_t req, uint32_t a)
{
    (void)a;
    if (req == 0x541b) { wr32(a, (uint32_t)(drive_n - drive_pos)); return 0; }   // FIONREAD
    return 0;                                       // termios settings
}

static int drive_write(uint32_t buf, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)G(buf);
#ifdef LB_DRIVE_STATUS
    // a packet per write: 4 bytes for one wheel, 7 for a twin cabinet's two (two data
    // bytes each); a status byte per wheel
    if (n >= 4 && (p[0] & 0x80)) {
        if (hle_verbose) hle_log("drive: %02x (%u bytes)\n", p[0], n);
        if (drive_pos == drive_n) drive_n = drive_pos = 0;
        uint8_t st = (p[0] & 0x7f) == 0x01 ? 1 : 0;
        for (uint32_t w = 0; w < (n - 1) / 3 && drive_n < (int)sizeof drive_out; w++)
            drive_out[drive_n++] = (uint8_t)(st << 4 | st);
    }
    return (int)n;
#endif
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] & 0x80) drive_cmd_n = 0;
        if (drive_cmd_n < 4) drive_cmd[drive_cmd_n++] = p[i];
        if (drive_cmd_n == 4 && drive_n + 4 <= (int)sizeof drive_out) {
            if (hle_verbose) hle_log("drive: %02x %02x %02x\n", drive_cmd[0], drive_cmd[1], drive_cmd[2]);
            if (drive_pos == drive_n) drive_n = drive_pos = 0;
            uint8_t *r = drive_out + drive_n;
            int pos = 0x2000;                       // centred
            uint8_t hi = (uint8_t)(pos >> 7 & 0x7f), lo = (uint8_t)(pos & 0x7f);
            r[0] = 0x94; r[1] = hi; r[2] = lo; r[3] = (uint8_t)(0x14 ^ hi ^ lo);
            drive_n += 4;
            drive_cmd_n = 0;
        }
    }
    return (int)n;
}

static int drive_read(uint32_t buf, uint32_t n)
{
    int m = drive_n - drive_pos;
    if (m > (int)n) m = (int)n;
    memcpy(G(buf), drive_out + drive_pos, m);
    drive_pos += m;
    return m;
}

static int drive_select(void) { return drive_n > drive_pos; }

static const device_ops drive_dev = {"/dev/tts/0", drive_open, drive_ioctl, drive_read, drive_write, drive_select};

// ---- the second serial port: /dev/tts/1 ------------------------------------------------
// OutRun 2 SP's ACU (15-byte frames from 0xff with an XOR check): written to, never
// answering, which the game takes as the unit being absent
static int quiet_open(void) { return 0; }
static int quiet_ioctl(uint32_t req, uint32_t a)
{
    if (req == 0x541b) wr32(a, 0);                  // FIONREAD
    return 0;                                       // termios settings
}
static int quiet_read(uint32_t buf, uint32_t n) { (void)buf; (void)n; return 0; }
static int quiet_write(uint32_t buf, uint32_t n) { (void)buf; return (int)n; }
static int quiet_select(void) { return 0; }
static const device_ops tts1_dev = {"/dev/tts/1", quiet_open, quiet_ioctl, quiet_read, quiet_write, quiet_select};

// ---- files the cabinet's kernel provides -----------------------------------------------
// PCI configuration space of the ICH LPC bridge (bus 0, device 31, function 0). The
// game reads the ACPI (0x40) and GPIO (0x58) I/O base registers from it.
static uint8_t lpc_config[256];

// The test menu draws with a font from the cabinet's system disk, which is not
// part of the game data. Stand in a 256x128 atlas (16 glyphs a row, from ' ')
// with the character table the menu reads: a 28-byte header, then for each
// character from ' ' the texture rectangle (4 floats) and three shorts whose
// ratio b/(a+c) scales the cell width (0,1,1 = the full cell).
#include "boot_font.h"
#define ATLAS_W 256
#define ATLAS_H 128
static uint8_t font_tga[18 + ATLAS_W * ATLAS_H * 4], font_abc[28 + 128 * 24], logo_tga[18 + 4 * 4 * 4];

static void tga_header(uint8_t *h, int w, int t)
{
    h[2] = 2;                                   // uncompressed true colour
    h[12] = w, h[13] = w >> 8, h[14] = t, h[15] = t >> 8;
    h[16] = 32;
    h[17] = 8;                                  // 8 alpha bits
}

static void build_boot_files(void)
{
    tga_header(font_tga, ATLAS_W, ATLAS_H);
    tga_header(logo_tga, 4, 4);                 // a transparent logo
    uint8_t *px = font_tga + 18;
    for (int ch = BOOT_FONT_FIRST; ch <= 159; ch++) {
        int k = ch - BOOT_FONT_FIRST, gx = (k % 16) * BOOT_FONT_W, gy = (k / 16) * BOOT_FONT_H;
        if (ch <= BOOT_FONT_LAST)
            for (int y = 0; y < BOOT_FONT_H; y++)
                for (int x = 0; x < BOOT_FONT_W; x++) {
                    int i = y * BOOT_FONT_W + x;
                    int a = (boot_font[k][i / 2] >> (i & 1 ? 0 : 4)) & 15;
                    uint8_t *p = px + ((gy + y) * ATLAS_W + gx + x) * 4;
                    p[0] = p[1] = p[2] = 255;
                    p[3] = a * 17;
                }
        else
            gx = gy = 0;                        // unprintable: the blank space cell
        float r[4] = {(float)gx / ATLAS_W, (float)gy / ATLAS_H,
                      (float)(gx + BOOT_FONT_W) / ATLAS_W, (float)(gy + BOOT_FONT_H) / ATLAS_H};
        int16_t abc[3] = {0, 1, 1};
        uint8_t *e = font_abc + 28 + k * 24;
        memcpy(e, r, 16);
        memcpy(e + 16, abc, 6);
    }
}

int virtual_file(const char *path, const void **data, size_t *size)
{
    if (!strncmp(path, "/usr/lib/boot/", 14)) {
        if (!font_tga[2]) build_boot_files();
        const char *n = path + 14;
        if (!strcmp(n, "LucidaConsole_12.tga")) *data = font_tga, *size = sizeof font_tga;
        else if (!strcmp(n, "LucidaConsole_12.abc")) *data = font_abc, *size = sizeof font_abc;
        else if (!strcmp(n, "logo.tga")) *data = logo_tga, *size = sizeof logo_tga;
        else return 0;
        return 1;
    }
    if (!strcmp(path, "/proc/bus/pci/00/1f.0")) {
        if (!lpc_config[0]) {
            lpc_config[0] = 0x86;            // vendor Intel
            lpc_config[1] = 0x80;
            lpc_config[2] = 0x40;            // 82801FB (ICH6) LPC bridge
            lpc_config[3] = 0x26;
            lpc_config[0x0a] = 0x01;         // class: ISA bridge
            lpc_config[0x0b] = 0x06;
            lpc_config[0x0e] = 0x80;         // multi-function
            lpc_config[0x40] = 0x01;         // PMBASE 0x1000, I/O space
            lpc_config[0x41] = 0x10;
            lpc_config[0x44] = 0x10;         // ACPI enabled
            lpc_config[0x58] = 0x81;         // GPIOBASE 0x1180, I/O space
            lpc_config[0x59] = 0x11;
            lpc_config[0x5c] = 0x10;         // GPIO enabled
        }
        *data = lpc_config;
        *size = sizeof lpc_config;
        return 1;
    }
    return 0;
}

const device_ops *device_for_path(const char *p)
{
    if (!strcmp(p, lbb_dev.path)) return &lbb_dev;
    if (!strcmp(p, i2c_dev.path)) return &i2c_dev;
    if (!strcmp(p, drive_dev.path)) return &drive_dev;
    if (!strcmp(p, tts1_dev.path)) return &tts1_dev;
    return NULL;
}

// ---- port I/O: security board, dongle, DIP switches ----------------------------------
// The functions that talk to these are replaced below, so stray accesses only log.
void guest_port_out(CPU *c, uint16_t port, uint32_t v, int size)
{
    (void)c;
    if (hle_verbose) hle_log("out 0x%x <- 0x%x (%d)\n", port, v, size);
}

uint32_t guest_port_in(CPU *c, uint16_t port, int size)
{
    (void)c;
    if (hle_verbose) hle_log("in 0x%x (%d)\n", port, size);
    return 0xffffffffu >> (32 - 8 * size);
}

// ---- replaced game functions (ovr_<symbol>) --------------------------------------------

static uint32_t object_addr(const char *name)
{
    for (unsigned i = 0; i < prog->obj_count; i++)
        if (!strcmp(prog->obj_names[i], name)) return prog->obj_addr[i];
    return 0;
}

// DIP switches: 1280x768 output (switches 4 and 5 on), horizontal monitor
static uint8_t dip_bits = 0x60;

void ovr_amDongleInit(CPU *c) { RET(0); }
void ovr_amDongleIsAvailable(CPU *c) { RET(1); }
void ovr_amDongleUpdate(CPU *c) { RET(0); }
void ovr_amDipswInit(CPU *c)
{
    uint32_t ctx = object_addr("amDipswContext");
    if (ctx) wr32(ctx, 1);
    RET(0);
}
void ovr_amDipswExit(CPU *c) { RET(0); }
void ovr_amDipswGetData(CPU *c)
{
    wr8(ARG(0), dip_bits);
    RET(0);
}
void ovr_amDipswSetLed(CPU *c) { RET(0); }
void ovr_XF86VidModeSwitchToMode(CPU *c) { RET(0); }

// Sega Race TV's own gettid makes the system call (int 0x80)
void ovr_gettid(CPU *c) { RET(1000); }
// The region the front end chose (afcore set_option "region", the core's
// options): 0 Export, 1 USA, 2 Japan. The games keep theirs in the system
// data of their EEPROM, which the runtime writes (eeprom_set_region); Race
// TV and Let's Go Jungle are also answered directly.
int lb_region;
// Sega Race TV (CGameAssign::GetGameRegion): 0 Japan, 1 USA, 2 Export; the
// language follows (English but in Japan)
void ovr__ZN11CGameAssign13GetGameRegionEv(CPU *c)
{
    static const int region[3] = {2, 1, 0};
    RET(region[lb_region]);
}
// Let's Go Jungle (acpSystem::getRegion): 1 Japan, 2 USA, 3 Export ("Lost on
// the Island of Spice")
void ovr__ZN9acpSystem9getRegionEv(CPU *c)
{
    static const int region[3] = {3, 2, 1};
    RET(region[lb_region]);
}

// CRI Sofdec's 8x8 inverse DCT (DCT_IsseTrans: src, dst, full), natively
// and to the bit (sofdec_idct.h): the game's MMX one, translated, was a large
// share of a frame while a movie plays (#49). Its tables' addresses in this
// game: games/<game>/game.cmake's GAME_SOFDEC_IDCT.
#ifdef LB_SOFDEC_IDCT
#include "sofdec_idct.h"
void ovr_DCT_IsseTrans(CPU *c)
{
    static const uint32_t at[12] = {LB_SOFDEC_IDCT};
    static const int16_t *t[12];
    if (!t[0])
        for (int i = 0; i < 12; i++) t[i] = (const int16_t *)G(at[i]);
    sofdec_idct((const int16_t *)G(ARG(0)), (int16_t *)G(ARG(1)), ARG(2) != 0, t);
    RET(0);
}
#endif

// The keychip's user data (64 bytes); Let's Go Jungle reads it at boot and
// stops with "Error 15, Game Program Not Found." unless it starts with its
// game ID. (handle, size, buffer)
#ifndef LB_KEYCHIP_ID
#define LB_KEYCHIP_ID ""
#endif
void ovr_amDongleUserInfoEx(CPU *c)
{
    uint32_t n = ARG(1) < 64 ? ARG(1) : 64;
    memset(G(ARG(2)), 0, n);
    memcpy(G(ARG(2)), LB_KEYCHIP_ID, strlen(LB_KEYCHIP_ID) < n ? strlen(LB_KEYCHIP_ID) : n);
    RET(0);
}

// Let's Go Jungle's full-screen setup asks the XF86VidMode extension for the
// current mode before it creates its window: one 1360x768 mode, as the screen
static void put_mode(uint32_t at)   // 9 shorts: h display/sync start/sync end/total/skew, v display/...
{
    static const uint16_t m[9] = {1360, 1424, 1536, 1792, 0, 768, 771, 777, 795};
    for (int i = 0; i < 9; i++) wr16(at + 2 * i, m[i]);
}
void ovr_XF86VidModeGetModeLine(CPU *c)
{
    if (ARG(2)) wr32(ARG(2), 85500);   // dot clock, kHz
    memset(M + ARG(3), 0, 32);
    put_mode(ARG(3));
    RET(1);
}
void ovr_XF86VidModeGetAllModeLines(CPU *c)
{
    uint32_t info = guest_calloc(1, 36), list = guest_calloc(1, 4);
    wr32(info, 85500);
    put_mode(info + 4);
    wr32(list, info);
    wr32(ARG(2), 1);
    wr32(ARG(3), list);
    RET(1);
}
void ovr_XF86VidModeGetViewPort(CPU *c)
{
    wr32(ARG(2), 0);
    wr32(ARG(3), 0);
    RET(1);
}
void ovr_XF86VidModeSetViewPort(CPU *c) { RET(1); }

// Let's Go Jungle's debug console (printf-like; silent in the shipped game)
void ovr__ZN7_sDebug10putConsoleEPKcz(CPU *c)
{
    char buf[4096];
    guest_vformat(buf, sizeof buf, G(ARG(0)), c->r[ESP] + 8);
    size_t n = strlen(buf);
    hle_log("console: %s%s", buf, n && buf[n - 1] == '\n' ? "" : "\n");
    RET(0);
}

// OutRun 2 SP's frame clock (milliseconds since SetFrameStartCpuTime, after the
// vsync). Its file loader inflates a file until 14.6 ms into the frame, and the
// frame's drawing comes first; on the Lindbergh the GPU drew while the CPU went
// on, but here the OpenGL calls themselves take the CPU's time (gl4es on iOS),
// and where drawing alone used up the 14.6 ms (the car selector on an iPad)
// the loader never took a step and the selector stayed empty. The clock
// leaves out the time spent in OpenGL.
static double frame_start;
void ovr__Z20SetFrameStartCpuTimev(CPU *c)
{
    gl_clock = 1;
    frame_start = hle_cpu_now();
    RET(0);
}
void ovr__Z13GetNowCpuTimev(CPU *c) { RETF((hle_cpu_now() - frame_start) * 1e3); }

static void h_zero(CPU *c) { RET(0); }

static const hle_entry lindbergh_table[] = {
    {"kswap_collect", h_zero},
    {NULL, NULL},
};

void hle_lindbergh_init(void)
{
    const char *s = af_debug_env("LB_SAVE_DIR");
    if (save_root[0]) snprintf(save_dir, sizeof save_dir, "%s", save_root);
    else if (s) snprintf(save_dir, sizeof save_dir, "%s", s);
    else snprintf(save_dir, sizeof save_dir, "%s/save", game_root);
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s/tmp", save_root[0] ? save_root : game_root);
    mkdir(save_dir, 0755);
    mkdir(tmp, 0755);
    const char *dip = af_debug_env("LB_DIPSW");
    if (dip) dip_bits = (uint8_t)strtoul(dip, NULL, 0);
#ifdef LB_DRIVING
    jvs_in.analog[0] = 0x8000;   // the wheel at its centre (OutRun 2 SP homes the drive board on it)
#endif
}

// a core stopped the game: write the saves and put the cabinet back to power on
void hle_lindbergh_reset(void)
{
    if (sram) fclose(sram);
    if (eeprom) fclose(eeprom);
    sram = eeprom = NULL;
    jvs_addr = -1;
    sense = 3;
    jvs_last_n = 0;
    memset(shm, 0, sizeof shm);
    shm_pos = 0;
    lbb_ready = -1;
    jvs_dest = jvs_size = serial_dest = 0;
    memset(lpc_config, 0, sizeof lpc_config);
    dip_bits = 0x60;
    memset(&jvs_in, 0, sizeof jvs_in);
#ifdef LB_DRIVING
    jvs_in.analog[0] = 0x8000;   // the wheel at its centre (OutRun 2 SP homes the drive board on it)
#endif
}

// the save files as they are now, for a core's save()
void hle_lindbergh_flush(void)
{
    if (sram) fflush(sram);
    if (eeprom) fflush(eeprom);
}

__attribute__((constructor)) static void reg(void) { hle_register(lindbergh_table); }
