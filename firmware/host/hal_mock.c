/*
 * Host-side HAL: simulates BOTH the programmer pins and the AVR target
 * sitting in the socket.  Rather than just logging control-stack indices,
 * it decodes the actual OE/WR/XA/BS waveforms per the AVR HVPP datasheet
 * semantics and serves signature/fuse/lock/flash/EEPROM bytes — so a green
 * gate run proves the engine produces electrically correct sequences.
 *
 * The simulated chip is wired STRAIGHT signal-to-signal.  For 20-pin and
 * x61-class parts the chip shares BS2 with XA1 and PAGEL with BS1 on single
 * pins (profile flag bs2_on_xa1), exactly like the real ATtiny461A rig.
 *
 * Env: SIM_PROFILE = t461a (default) | t2313 | m8;  SIM_VERBOSE = 1 for
 * per-pin-op logging.  All logs go to stderr, prefixed "SIM:".
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "hal.h"

int sim_warnings = 0;

static int verbose = 0;
static int tx_fd = -1;

#define LOGF(...)  do { fprintf(stderr, "SIM: " __VA_ARGS__); \
                        fputc('\n', stderr); } while (0)
#define VLOGF(...) do { if (verbose) LOGF(__VA_ARGS__); } while (0)
#define WARNF(...) do { fprintf(stderr, "SIM: WARN " __VA_ARGS__); \
                        fputc('\n', stderr); sim_warnings++; } while (0)

/* ---- simulated target ---- */

typedef struct {
    const char *name;
    uint8_t sig[3];
    uint8_t lfuse, hfuse, efuse, osccal;
    int bs2_on_xa1;           /* chip shares BS2 with XA1 line, PAGEL with BS1 */
    uint32_t flash_bytes, ee_bytes;
} profile_t;

static const profile_t profiles[] = {
    { "t461a", {0x1E,0x92,0x08}, 0x62, 0xDF, 0xFF, 0x8F, 1,  4096, 256 },
    { "t2313", {0x1E,0x91,0x0A}, 0x64, 0xDF, 0xFF, 0x66, 1,  2048, 128 },
    { "m8",    {0x1E,0x93,0x07}, 0xE1, 0xD9, 0xFF, 0xA0, 0,  8192, 512 },
};

static const profile_t *prof = &profiles[0];
static uint8_t fuse_l, fuse_h, fuse_e, lock_bits = 0xFF;
static uint8_t flash_mem[65536], ee_mem[4096];

/* programmer-side pin state */
static uint8_t ctrl_lines, data_out;
static int data_dir = 1;                 /* 1 = bus driven by programmer */
static int vcc_on, hv_on, progmode;
static int latch_pulses;

/* target-side latches */
static uint8_t cmd_latched, addr_lo, addr_hi, dbuf_lo, dbuf_hi;

static uint8_t flash_expected(uint32_t a) { return (uint8_t)(a ^ (a >> 8)); }
static uint8_t ee_expected(uint32_t a)    { return (uint8_t)((a ^ 0xA5) + (a >> 8)); }

static void
sim_summary(void)
{
    LOGF("exit: %d warning(s)", sim_warnings);
}

__attribute__((constructor)) static void
sim_init(void)
{
    const char *p = getenv("SIM_PROFILE");
    const char *v = getenv("SIM_VERBOSE");
    size_t i;
    uint32_t a;

    verbose = v && *v && *v != '0';
    if (p) {
        for (i = 0; i < sizeof profiles / sizeof profiles[0]; ++i)
            if (!strcmp(p, profiles[i].name))
                prof = &profiles[i];
        if (strcmp(prof->name, p))
            WARNF("unknown SIM_PROFILE '%s', using %s", p, prof->name);
    }
    fuse_l = prof->lfuse;
    fuse_h = prof->hfuse;
    fuse_e = prof->efuse;
    memset(flash_mem, 0xFF, sizeof flash_mem);
    memset(ee_mem, 0xFF, sizeof ee_mem);
    for (a = 0; a < prof->flash_bytes; ++a)
        flash_mem[a] = flash_expected(a);
    for (a = 0; a < prof->ee_bytes; ++a)
        ee_mem[a] = ee_expected(a);
    LOGF("profile %s (sig %02X%02X%02X, bs2_on_xa1=%d)",
         prof->name, prof->sig[0], prof->sig[1], prof->sig[2],
         prof->bs2_on_xa1);
    atexit(sim_summary);
}

/* decode the standard-mapped control byte as the straight-wired chip sees it */
static int sBS1(void) { return (ctrl_lines >> 4) & 1; }
static int sBS2(void) { return prof->bs2_on_xa1 ? (ctrl_lines >> 6) & 1
                                                : ctrl_lines & 1; }
static int sOE(void)  { return (ctrl_lines >> 2) & 1; }
static int sWR(void)  { return (ctrl_lines >> 3) & 1; }
static int sXA(void)  { return ((ctrl_lines >> 5) & 1) |
                               (((ctrl_lines >> 6) & 1) << 1); }

static const char *
cmd_name(uint8_t c)
{
    switch (c) {
    case 0x00: return "no-op";
    case 0x02: return "read flash";
    case 0x03: return "read eeprom";
    case 0x04: return "read fuse/lock";
    case 0x08: return "read signature/osccal";
    case 0x10: return "write flash";
    case 0x11: return "write eeprom";
    case 0x20: return "write lock";
    case 0x40: return "write fuse";
    case 0x80: return "chip erase";
    default:   return "?";
    }
}

static void
wr_commit(void)
{
    static const char *fnames[] = { "lfuse", "hfuse", "efuse" };
    int which;

    if (!progmode) {
        WARNF("WR pulse outside progmode");
        return;
    }
    switch (cmd_latched) {
    case 0x80:
        LOGF("CHIP ERASE (WR pulse)");
        memset(flash_mem, 0xFF, sizeof flash_mem);
        memset(ee_mem, 0xFF, sizeof ee_mem);
        lock_bits = 0xFF;
        break;
    case 0x40:
        which = sBS2() ? 2 : (sBS1() ? 1 : 0);
        LOGF("WRITE %s <- 0x%02X", fnames[which], dbuf_lo);
        if (which == 0) fuse_l = dbuf_lo;
        else if (which == 1) fuse_h = dbuf_lo;
        else fuse_e = dbuf_lo;
        break;
    case 0x20:
        LOGF("WRITE lock <- 0x%02X", dbuf_lo);
        lock_bits &= dbuf_lo;
        break;
    case 0x10:
        LOGF("WRITE flash page @0x%04X", ((unsigned)addr_hi << 8) | addr_lo);
        break;
    case 0x11:
        LOGF("WRITE eeprom page @0x%04X", ((unsigned)addr_hi << 8) | addr_lo);
        break;
    default:
        WARNF("WR pulse with command 0x%02X (%s)",
              cmd_latched, cmd_name(cmd_latched));
        break;
    }
}

/* ---- HAL implementation ---- */

void
hal_ctrl_set(uint8_t s)
{
    int wr_fall = ((ctrl_lines >> 3) & 1) && !((s >> 3) & 1);

    ctrl_lines = s;
    VLOGF("CTRL=0x%02X  OE=%d WR=%d BS1=%d BS2=%d XA=%d PAGEL=%d",
          s, sOE(), sWR(), sBS1(), sBS2(), sXA(), (s >> 7) & 1);
    if (wr_fall)
        wr_commit();
}

void
hal_ctrl_engage(void)
{
    LOGF("controls engaged (push-pull)");
}

void
hal_ctrl_release(void)
{
    LOGF("controls released (inputs)");
}

void
hal_data_mode(uint8_t output)
{
    data_dir = output;
    VLOGF("data bus %s", output ? "driven" : "high-Z (read)");
}

void
hal_data_set(uint8_t v)
{
    if (!data_dir)
        WARNF("data written while bus is high-Z");
    data_out = v;
    VLOGF("DATA<=0x%02X", v);
}

void
hal_xtal_pulse(void)
{
    if (!vcc_on) {
        WARNF("XTAL1 pulse with target VCC off");
        return;
    }
    if (!hv_on) {
        ++latch_pulses;    /* progmode-entry latch cycles */
        return;
    }
    switch (sXA()) {
    case 0:    /* load address */
        if (!data_dir)
            WARNF("address load with bus high-Z");
        if (sBS1()) {
            addr_hi = data_out;
            VLOGF("load addr high = 0x%02X", data_out);
        } else {
            addr_lo = data_out;
            VLOGF("load addr low  = 0x%02X", data_out);
        }
        break;
    case 1:    /* load data */
        if (sBS1()) dbuf_hi = data_out;
        else        dbuf_lo = data_out;
        VLOGF("load data %s = 0x%02X", sBS1() ? "high" : "low", data_out);
        break;
    case 2:    /* load command */
        cmd_latched = data_out;
        LOGF("CMD latched 0x%02X (%s)", data_out, cmd_name(data_out));
        break;
    case 3:    /* no action */
        break;
    }
}

uint8_t
hal_data_get(void)
{
    static const char *fnames[] = { "lfuse", "lock", "efuse", "hfuse" };
    uint8_t val = 0xFF;
    uint32_t a;
    int sel;

    if (data_dir) {
        WARNF("data read while programmer drives the bus");
        return data_out;
    }
    if (sOE()) {
        WARNF("data read with OE inactive (bus floating)");
        return 0xFF;
    }
    if (!progmode)
        WARNF("data read outside progmode");

    switch (cmd_latched) {
    case 0x04:
        sel = (sBS2() << 1) | sBS1();
        val = (sel == 0) ? fuse_l : (sel == 1) ? lock_bits
            : (sel == 2) ? fuse_e : fuse_h;
        LOGF("READ %s -> 0x%02X", fnames[sel], val);
        break;
    case 0x08:
        if (sBS1()) {
            val = prof->osccal;
            LOGF("READ osccal -> 0x%02X", val);
        } else {
            val = (addr_lo < 3) ? prof->sig[addr_lo] : 0xFF;
            LOGF("READ sig[%d] -> 0x%02X", addr_lo, val);
        }
        break;
    case 0x02:
        a = (((uint32_t)addr_hi << 8) | addr_lo) * 2 + (sBS1() ? 1 : 0);
        val = (a < sizeof flash_mem) ? flash_mem[a] : 0xFF;
        VLOGF("READ flash[0x%05X] -> 0x%02X", a, val);
        break;
    case 0x03:
        a = ((uint32_t)addr_hi << 8) | addr_lo;
        val = (a < sizeof ee_mem) ? ee_mem[a] : 0xFF;
        VLOGF("READ eeprom[0x%04X] -> 0x%02X", a, val);
        break;
    default:
        WARNF("data read with command 0x%02X (%s)",
              cmd_latched, cmd_name(cmd_latched));
        break;
    }
    return val;
}

/* ISP (serial programming) target sim — byte level; the AVR's shift
 * register echoes the PREVIOUS byte received, except data-out positions. */
static int isp_connected, isp_synced;
static uint8_t icmd[4], isp_last;
static int ipos;
static uint8_t page_buf[128];

void
hal_isp_connect(void)
{
    isp_connected = 1;
    isp_synced = 0;
    ipos = 0;
    LOGF("ISP connect (MISO in, SCK low, MOSI low)");
}

void
hal_isp_disconnect(void)
{
    isp_connected = 0;
    isp_synced = 0;
    LOGF("ISP disconnect");
}

static void
isp_exec(void)    /* side effects once a 4-byte command completes */
{
    uint32_t a;

    if (icmd[0] == 0xAC && icmd[1] == 0x53) {
        isp_synced = 1;
        LOGF("ISP: programming enable (synced)");
    } else if (!isp_synced) {
        WARNF("ISP command %02X %02X before programming enable",
              icmd[0], icmd[1]);
    } else if (icmd[0] == 0xAC && icmd[1] == 0x80) {
        LOGF("ISP: CHIP ERASE");
        memset(flash_mem, 0xFF, sizeof flash_mem);
        memset(ee_mem, 0xFF, sizeof ee_mem);
        lock_bits = 0xFF;
    } else if (icmd[0] == 0xAC && icmd[1] == 0xA0) {
        LOGF("ISP: WRITE lfuse <- 0x%02X", icmd[3]); fuse_l = icmd[3];
    } else if (icmd[0] == 0xAC && icmd[1] == 0xA8) {
        LOGF("ISP: WRITE hfuse <- 0x%02X", icmd[3]); fuse_h = icmd[3];
    } else if (icmd[0] == 0xAC && icmd[1] == 0xA4) {
        LOGF("ISP: WRITE efuse <- 0x%02X", icmd[3]); fuse_e = icmd[3];
    } else if (icmd[0] == 0xAC && icmd[1] == 0xE0) {
        LOGF("ISP: WRITE lock <- 0x%02X", icmd[3]); lock_bits &= icmd[3];
    } else if (icmd[0] == 0x40 || icmd[0] == 0x48) {
        a = ((icmd[2] & 0x3F) << 1) | (icmd[0] == 0x48 ? 1 : 0);
        page_buf[a] = icmd[3];
    } else if (icmd[0] == 0x4C) {
        a = (((uint32_t)icmd[1] << 8) | icmd[2]) * 2;
        LOGF("ISP: WRITE flash page @0x%05X", a);
        if (a + sizeof page_buf <= sizeof flash_mem)
            memcpy(flash_mem + a, page_buf, sizeof page_buf);
    } else if (icmd[0] == 0xC0) {
        a = (((uint32_t)icmd[1] & 0x0F) << 8) | icmd[2];
        LOGF("ISP: WRITE eeprom[0x%04X] <- 0x%02X", a, icmd[3]);
        if (a < sizeof ee_mem)
            ee_mem[a] = icmd[3];
    }
}

void
hal_isp_sck_pulse(void)
{
    ipos = 0;    /* bit/byte resync, as on a real AVR */
    VLOGF("ISP SCK sync pulse");
}

uint8_t
hal_isp_xfer(uint8_t b)
{
    uint8_t ret;

    if (!isp_connected)
        WARNF("ISP xfer while not connected");
    if (!vcc_on) {
        WARNF("ISP xfer with target VCC off");
        return 0xFF;
    }
    if (hv_on)
        WARNF("ISP xfer with 12V on RESET");

    ret = (ipos == 0) ? isp_last : icmd[ipos - 1];
    icmd[ipos] = b;

    if (ipos == 3) {    /* data-out position of read commands */
        uint32_t a;
        if (!isp_synced) {
            ret = 0xFF;
        } else if (icmd[0] == 0x30) {
            ret = (icmd[2] < 3) ? prof->sig[icmd[2]] : 0xFF;
            LOGF("ISP: READ sig[%d] -> 0x%02X", icmd[2], ret);
        } else if (icmd[0] == 0x50 && icmd[1] == 0x00) {
            ret = fuse_l;    LOGF("ISP: READ lfuse -> 0x%02X", ret);
        } else if (icmd[0] == 0x58 && icmd[1] == 0x08) {
            ret = fuse_h;    LOGF("ISP: READ hfuse -> 0x%02X", ret);
        } else if (icmd[0] == 0x50 && icmd[1] == 0x08) {
            ret = fuse_e;    LOGF("ISP: READ efuse -> 0x%02X", ret);
        } else if (icmd[0] == 0x58 && icmd[1] == 0x00) {
            ret = lock_bits; LOGF("ISP: READ lock -> 0x%02X", ret);
        } else if (icmd[0] == 0x38) {
            ret = prof->osccal;
        } else if (icmd[0] == 0x20 || icmd[0] == 0x28) {
            a = ((((uint32_t)icmd[1] << 8) | icmd[2]) << 1)
              | (icmd[0] == 0x28 ? 1 : 0);
            ret = (a < sizeof flash_mem) ? flash_mem[a] : 0xFF;
            VLOGF("ISP: READ flash[0x%05X] -> 0x%02X", a, ret);
        } else if (icmd[0] == 0xA0) {
            a = (((uint32_t)icmd[1] & 0x0F) << 8) | icmd[2];
            ret = (a < sizeof ee_mem) ? ee_mem[a] : 0xFF;
            VLOGF("ISP: READ eeprom[0x%04X] -> 0x%02X", a, ret);
        } else if (icmd[0] == 0xF0) {
            ret = 0x00;      /* never busy */
        }
    }

    isp_last = b;
    if (++ipos == 4) {
        ipos = 0;
        isp_exec();
    }
    return ret;
}

void
hal_vcc(uint8_t on)
{
    if (on && !vcc_on)
        latch_pulses = 0;
    if (!on) {
        progmode = 0;
        isp_synced = 0;    /* power loss drops serial progmode + bit sync */
        ipos = 0;
    }
    if (on != (uint8_t)vcc_on)
        LOGF("VCC %s", on ? "ON" : "off");
    vcc_on = on;
}

void
hal_hv_reset(uint8_t apply12v)
{
    if (apply12v && !hv_on) {
        if (!vcc_on)
            WARNF("12V applied before target VCC");
        LOGF("RESET -> 12V (progmode entry, %d latch pulse(s) seen)",
             latch_pulses);
        progmode = 1;
    } else if (!apply12v && hv_on) {
        LOGF("RESET -> GND");
        progmode = 0;
    }
    hv_on = apply12v;
}

uint8_t
hal_rdy_wait(uint8_t timeout_ms)
{
    (void)timeout_ms;
    if (!progmode)
        WARNF("RDY poll outside progmode");
    VLOGF("RDY poll -> ready");
    return 1;
}

void hal_delay_ms(uint8_t ms) { (void)ms; }
void hal_delay_us(uint8_t us) { (void)us; }

/* ---- transport ---- */

static uint8_t txbuf[600];
static size_t txlen;

void
mock_set_tx_fd(int fd)
{
    tx_fd = fd;
}

void
hal_tx_byte(uint8_t b)
{
    if (txlen < sizeof txbuf)
        txbuf[txlen++] = b;
}

void
hal_tx_flush(void)
{
    size_t off = 0;

    if (tx_fd < 0)
        return;    /* no transport: keep buffer for mock_take_tx() */
    while (off < txlen) {
        ssize_t n = write(tx_fd, txbuf + off, txlen - off);
        if (n <= 0) {
            WARNF("tx write failed");
            break;
        }
        off += (size_t)n;
    }
    txlen = 0;
}

/* for test programs that want to inspect the response */
size_t
mock_take_tx(uint8_t *dst, size_t max)
{
    size_t n = txlen < max ? txlen : max;
    memcpy(dst, txbuf, n);
    txlen = 0;
    return n;
}

void
hal_led(uint8_t on)
{
    (void)on;
}

int mock_isp_entries;

void
hal_enter_isp(void)
{
    ++mock_isp_entries;
    LOGF("ISP re-entry requested (would set IAP_CONTR=0x60)");
}
