/*
 * MustangDash - Riverdi 7" EVE4 panel on a Teensy 4.1
 *
 * Display : Riverdi SM-RVT70HSBNWN00 (7.0" 1024x600 IPS, BT817 / EVE4, no touch)
 * MCU     : Teensy 4.1 (IMXRT1062)
 * Bus     : hardware SPI0  (SCLK=13, MISO=12, MOSI=11)
 * Control : CS = 14, PD/RST = 17   (INT not connected -> polling only)
 *
 * Library : RudolphRiedel/FT800-FT813 (EmbeddedVideoEngine, v5.x)
 *           profile EVE_RVT70H, pins set in EVE_target_Arduino_Teensy4.h
 *
 * What it does:
 *   1. waits for the serial monitor only when a USB host is present
 *      (usb_configuration != 0); a car boot spends at most the bounded
 *      500 ms enumeration window before display init, not the 2 s wait
 *   2. brings the EVE chip out of power-down, runs EVE_init(), reads REG_ID
 *      (a healthy BT817 returns 0x7C) and reports both on Serial
 *   3. checks the splash ASTC assets in the panel's QSPI flash against the
 *      embedded pack (splash_flash.h) and provisions them once on mismatch
 *      (CMD_FLASHUPDATE, sector 0 never touched); inflates the dash fonts
 *      into RAM_G (dash_fonts.h) -- all with the backlight held dark
 *   4. plays the 2000 ms animated splash (assets/splash/README.md spec,
 *      timing in splash_timeline.h; theme picked in splash_config.h),
 *      rendering ASTC bitmaps directly from flash, lighting the backlight
 *      only after the first frame is on screen
 *   5. crossfades ~400 ms into the dash -- TRACK or STREET view fed by the
 *      built-in simulator (dash_sim.h) with serial overrides (dash_serial.h),
 *      alarm takeover on critical conditions, EEPROM-persisted odometer --
 *      the standing display, redrawn continuously from loop()
 *
 * Serial is 115200 8N1. Boot prints a diagnostic banner; after boot the
 * firmware emits nothing except one `ok ...` / `err ...` ack per received
 * command line (see dash_serial.h for the protocol; /dash skill wraps it).
 *
 * Rendering lives in two sibling headers (pure code motion, one translation
 * unit): dash_render.h (TRACK/STREET/alarm + gauge helpers) and
 * splash_render.h (flash provisioning + splash animation + crossfade).
 * This file keeps setup/loop and the glue: EVE frame plumbing, backlight,
 * fonts, serial pump, odometer EEPROM, and the shared state both headers use.
 *
 * Note: this project also builds offline with a minimal arduino-cli Teensy
 * platform that does not run the arduino .ino prototype generator, so every
 * function below is declared before it is used.
 */

#include <Arduino.h>
#include <SPI.h>
#include <avr/eeprom.h> /* Teensy 4.1 wear-leveled EEPROM emulation (4284 B) */
#include "EVE.h"
#include "splash_config.h"
#include "splash_timeline.h"
#include "splash_flash.h" /* after EVE.h so its guarded EVE_ASTC_* defer to the library */
#include "dash_data.h"
#include "dash_math.h"
#include "dash_layout.h"
#include "dash_sim.h"
#include "dash_serial.h"
#include "dash_odo.h"
#include "dash_fonts.h"

/* Teensy 4 USB device state: non-zero once the USB host has configured us
 * (cores/teensy4/usb.c). Zero when running from a wall adapter / car power. */
extern "C" {
extern volatile uint8_t usb_configuration;
}

/* Volatile so the compiler cannot fold the table access down to the one
 * compiled theme -- all three theme rows must stay live (plan requirement
 * R5; the flash pack always carries all three asset sets anyway). */
static volatile uint8_t g_theme = SPLASH_THEME;

/* ---- dash state ---- */

/* Fonts occupy RAM_G from address 0 (~273 KB decoded; dash_fonts.h footer
 * has the exact figure). The splash renders from QSPI flash, so fonts are
 * RAM_G's only standing tenant. */
struct DashFontLoaded
{
    uint32_t metrics_addr; /* 148-byte block, passed to CMD_SETFONT2 */
    bool ok;               /* false -> renderers fall back to ROM font 31 */
};

static DashFontLoaded g_fonts[DASH_FONT_COUNT];
static DashState g_dash;
static DashSimState g_sim;
static DashOdo g_odo;

static char g_serial_line[DASH_SERIAL_MAX_LINE + 17]; /* headroom to detect too-long */
static uint8_t g_serial_len = 0U;

static uint32_t g_loop_last_ms = 0UL;
static uint16_t g_fps = 0U;         /* frames completed in the last full second */
static uint16_t g_fps_frames = 0U;
static uint32_t g_fps_window_ms = 0UL;
static uint16_t g_dl_track = 0U;    /* boot-measured DL usage per mode (bytes/4) */
static uint16_t g_dl_street = 0U;
static uint32_t g_eve_faults = 0UL; /* coprocessor faults auto-recovered by EVE_busy() */

/* ---- colours (0xRRGGBB, design tokens from assets/dash-design/README.md) ---- */
static const uint32_t COLOR_BG        = 0x080B0FUL; /* flat panel fill (spec-sanctioned) */
static const uint32_t COLOR_ACCENT    = 0xE8A33CUL; /* gold: arcs, RPM fill */
static const uint32_t COLOR_GREEN     = 0x3DDF77UL; /* ok / delta ahead */
static const uint32_t COLOR_AMBER     = 0xF2B13EUL; /* warn */
static const uint32_t COLOR_RED_FILL  = 0xFF3B3BUL; /* alert fills / LEDs */
static const uint32_t COLOR_RED_TEXT  = 0xFF5252UL; /* alert text */
static const uint32_t COLOR_VALUE     = 0xF4F7F9UL; /* primary numerals */
static const uint32_t COLOR_VALUE_DIM = 0xC3CCD4UL; /* secondary values */
static const uint32_t COLOR_ODO       = 0x8B97A3UL; /* odometer value */
static const uint32_t COLOR_LABEL     = 0x5F6B76UL; /* small caps labels */
static const uint32_t COLOR_FAINT     = 0x454F59UL; /* footer / compressed ticks */
static const uint32_t COLOR_TICK_DIM  = 0x49545FUL; /* dim knee-region tick labels */
static const uint32_t COLOR_HAIRLINE  = 0x151B22UL; /* dividers */
static const uint32_t COLOR_ARC_TRACK = 0x141B23UL; /* gauge background arc */
static const uint32_t COLOR_BAR_TRACK = 0x0C1117UL; /* RPM bar track / hub disc */
static const uint32_t COLOR_REDZONE   = 0x5C1616UL; /* static tach red-zone arc */
static const uint32_t COLOR_MUTED_RED = 0x864B4BUL; /* the muted "8" scale mark */
static const uint32_t COLOR_BEST      = 0xB79AFFUL; /* best-lap purple */
static const uint32_t COLOR_ALARM_TXT = 0xFFD9D9UL; /* alarm header/limit text */
static const uint32_t COLOR_HUB_RING  = 0x39434DUL; /* gauge hub ring / dead labels */
static const uint32_t COLOR_DEAD_DOT  = 0x161D25UL; /* telltale dead-front dot */
static const uint32_t COLOR_NODATA    = 0x2A323BUL; /* telltale "no data" state (KTD4) */

/* steady backlight duty: 128 = 100% (the EVE PWM scale tops out at 128) */
static const uint8_t BL_STEADY = 128U;

static bool eve_ready = false; /* set true only if EVE_init() reported E_OK */

/* ---- forward declarations (explicit prototypes, see note above) ---- */
void set_backlight(uint8_t duty);
void eve_frame_begin(uint32_t clear_rgb);
void eve_frame_end(void);
bool load_dash_fonts(void);
uint16_t dash_font(uint8_t idx);
void dash_register_fonts(void);
void odo_eeprom_load(void);
void odo_eeprom_write(void);
void pump_serial(void);
void handle_serial_line(const char *line);

/* Renderers (pure code motion out of this file): dash first, then splash --
 * run_splash() calls draw_dash_content() during the crossfade. Both read the
 * shared state and glue prototypes above. */
#include "dash_draw.h"
#include "dash_render.h"
#include "splash_render.h"

void setup(void)
{
    Serial.begin(115200);

    /* Wait for the serial monitor only when a USB host is actually there:
     * enumeration takes ~100-300 ms when a host is present, so give it a
     * bounded 500 ms window; only then is the longer wait for the monitor
     * (DTR) worth it. From a wall adapter / car supply, usb_configuration
     * stays 0 and the loop exits at its 500 ms timeout -- the car boot
     * cost is that bounded window, not the 2 s monitor wait. */
    uint32_t t_start = millis();
    while ((0U == usb_configuration) && ((millis() - t_start) < 500U))
    {
        /* wait for USB enumeration, briefly */
    }
    if (0U != usb_configuration)
    {
        while (!Serial && ((millis() - t_start) < 2000U))
        {
            /* host present: wait up to 2 s total for the monitor to open */
        }
    }

    Serial.println();
    Serial.println(F("=== MustangDash / Riverdi RVT70H (BT817) on Teensy 4.1 ==="));
    Serial.printf("Pins: EVE_CS=%d  EVE_PDN=%d  (SCLK=13 MISO=12 MOSI=11)\r\n",
                  (int)EVE_CS, (int)EVE_PDN);
    Serial.printf("Panel: %lu x %lu, EVE_GEN=%d\r\n",
                  (unsigned long)EVE_HSIZE, (unsigned long)EVE_VSIZE, (int)EVE_GEN);
    Serial.printf("Splash theme: %u (0=blue 1=red 2=checkered)\r\n", (unsigned)g_theme);

    /* drive the control pins from the MCU */
    pinMode(EVE_CS, OUTPUT);
    digitalWrite(EVE_CS, HIGH); /* CS idle high */
    pinMode(EVE_PDN, OUTPUT);
    digitalWrite(EVE_PDN, LOW); /* hold in power-down until EVE_init() sequences it */

    /* SPI mode 0, MSB first; keep the clock conservative for init (BT817 needs
     * <= 11 MHz until the chip is configured). 8 MHz is safe and stays fast
     * enough for these small per-frame display lists, so we leave it here. */
    SPI.begin();
    SPI.beginTransaction(SPISettings(8UL * 1000000UL, MSBFIRST, SPI_MODE0));
    Serial.println(F("SPI up at 8 MHz, mode 0."));

    Serial.print(F("Running EVE_init()... "));
    uint8_t ret = EVE_init(); /* powers up, configures the RVT70H timings, enables PCLK */
    Serial.printf("returned 0x%02X (E_OK = 0x%02X)\r\n", ret, (uint8_t)E_OK);

    /* Read the chip id register. A healthy BT81x reports 0x7C. This is the
     * quickest way to confirm the SPI link from the serial monitor. */
    uint8_t reg_id = EVE_memRead8(REG_ID);
    Serial.printf("REG_ID = 0x%02X (expected 0x7C)\r\n", reg_id);

    /* Probe the panel's onboard QSPI flash (dash plan KTD11): attach and
     * switch to fast mode. REG_FLASH_SIZE reports the detected capacity in
     * megabytes. A failure here is logged, never fatal -- the dash must
     * not depend on flash. */
    uint8_t flash_ret = EVE_init_flash();
    uint32_t flash_mb = EVE_memRead32(REG_FLASH_SIZE);
    Serial.printf("EVE_init_flash() returned 0x%02X (E_OK=0x00), REG_FLASH_SIZE = %lu MB\r\n",
                  flash_ret, (unsigned long)flash_mb);

    /* Odometer loads regardless of panel state -- it is Teensy-local. */
    dash_state_init(&g_dash);
    dash_sim_init(&g_sim);
    dash_odo_init(&g_odo);
    odo_eeprom_load();
    Serial.printf("Odometer: %.1f mi (trip %.1f)\r\n",
                  (double)dash_odo_miles(&g_odo), (double)dash_trip_miles(&g_odo));

    if (E_OK == ret)
    {
        eve_ready = true;

        /* Backlight stays dark through flash provisioning + font upload so
         * power-up never flashes a lit blank panel; run_splash() lights it
         * once the first splash frame is on screen. */
        set_backlight(0U);

        /* Splash needs its ASTC assets in the panel's QSPI flash; verify
         * (and provision on first boot / asset change). Skipped -- along
         * with the splash itself -- if the flash probe above failed: the
         * dash must never depend on flash. */
        bool splash_ok = false;
        if (E_OK == flash_ret)
        {
            splash_ok = splash_flash_provision();
        }
        else
        {
            Serial.println(F("QSPI flash unavailable -> skipping splash (assets live in flash)."));
        }

        /* Dash fonts into RAM_G before the splash starts (KTD3): the upload
         * replaces the old splash-PNG decode in the boot budget, so
         * time-to-splash stays roughly what it was. */
        load_dash_fonts();

        /* Seed the first visible dash frame: everything starts invalid,
         * then one sim step fills plausible values -- an uninitialized
         * channel can never flash a false alarm at power-up (KTD3). */
        g_loop_last_ms = millis();
        dash_sim_step(&g_sim, &g_dash, 50U);

        /* One-time DL-usage diagnostic per mode (KTD8): built un-swapped
         * while the panel still shows nothing, printed into the banner --
         * after boot the only serial output is command acks. */
        g_dl_track = measure_mode_dl(DASH_MODE_TRACK);
        g_dl_street = measure_mode_dl(DASH_MODE_STREET);
        Serial.printf("DL usage: track %u/2048 words, street %u/2048\r\n",
                      (unsigned)g_dl_track, (unsigned)g_dl_street);

        if (splash_ok)
        {
            const ThemeDesc *theme = &THEMES[g_theme];
            run_splash(theme); /* 2000 ms animation + 400 ms crossfade into the dash */
        }

        set_backlight(BL_STEADY); /* no-op after the splash; lights the panel if it was skipped */
        Serial.println(F("EVE init OK: dash live. Serial is command-only from here (try 'help')."));
    }
    else
    {
        Serial.println(F("EVE_init() did NOT return E_OK - dash rendering disabled."));
        Serial.println(F("Serial commands still ack ('status' reports the failure). Check wiring / power / SPI."));
    }

    g_loop_last_ms = millis();
    g_fps_window_ms = g_loop_last_ms;
}

void loop(void)
{
    /* The live pipeline (KTD8): serial -> sim -> odometer -> alarm -> frame.
     * Runs even when the panel is dead (eve_ready false) so the bench
     * control surface survives -- only the render step is gated. */
    pump_serial();

    const uint32_t now = millis();
    const uint32_t dt = now - g_loop_last_ms;
    g_loop_last_ms = now;

    dash_sim_step(&g_sim, &g_dash, dt); /* honors sim_frozen + overrides */

    if (dash_ch_valid(&g_dash, DASH_CH_SPEED))
    {
        dash_odo_advance(&g_odo, g_dash.ch.speed_mph, dt);
    }
    if (dash_odo_should_write(&g_odo))
    {
        odo_eeprom_write();
        dash_odo_mark_written(&g_odo);
    }

    if (!eve_ready)
    {
        return;
    }

    dash_frame(now);

    /* fps accounting for the status ack: frames completed per full second */
    g_fps_frames++;
    if ((now - g_fps_window_ms) >= 1000UL)
    {
        g_fps = g_fps_frames;
        g_fps_frames = 0U;
        g_fps_window_ms = now;
    }
}

/* Write the backlight PWM duty (0..128 is the full range on EVE). */
void set_backlight(uint8_t duty)
{
    EVE_memWrite8(REG_PWM_DUTY, duty);
}

/* Open a new display list: clear color/stencil/tag to the given background. */
void eve_frame_begin(uint32_t clear_rgb)
{
    EVE_cmd_dl(CMD_DLSTART);
    EVE_cmd_dl(DL_CLEAR_COLOR_RGB | clear_rgb);
    EVE_cmd_dl(DL_CLEAR | CLR_COL | CLR_STN | CLR_TAG);
}

/* Close the display list, swap it in, and wait for the co-processor.
 * EVE_busy() detects and auto-recovers coprocessor faults but the recovery
 * is silent inside EVE_execute_cmd(); spinning here instead lets us count
 * recoveries so `status` can surface faults=N (review finding). */
void eve_frame_end(void)
{
    EVE_cmd_dl(DL_DISPLAY);
    EVE_cmd_dl(CMD_SWAP);
    for (;;)
    {
        const uint8_t st = EVE_busy();
        if (E_OK == st)
        {
            break;
        }
        if (EVE_FAULT_RECOVERED == st)
        {
            g_eve_faults++;
        }
    }
}

/* Inflate every dash font into RAM_G from address 0 and patch each metric
 * block's glyph pointer (KTD1). Success is verified with CMD_GETPTR: the
 * coprocessor reports the inflate end address, which must equal the packed
 * glyph size exactly. One retry per instance; a failed instance falls back
 * to ROM font 31 at render time (a degraded dash beats a black panel). */
bool load_dash_fonts(void)
{
    uint32_t addr = 0UL;
    bool all_ok = true;

    for (uint8_t i = 0U; i < DASH_FONT_COUNT; i++)
    {
        const DashFontDesc *f = &DASH_FONTS[i];
        const uint32_t maddr = addr;
        const uint32_t gaddr = (maddr + 148UL + 3UL) & ~3UL;
        const uint32_t gend_expected = gaddr + f->glyph_bytes;
        bool ok = false;

        for (uint8_t attempt = 0U; (attempt < 2U) && !ok; attempt++)
        {
            EVE_cmd_inflate(gaddr, f->glyphs_z, f->zbytes);
            EVE_execute_cmd();
            ok = (EVE_cmd_getptr() == gend_expected);
        }

        if (ok)
        {
            uint8_t block[148];
            memcpy(block, f->metrics, 148U);
            block[144] = (uint8_t)(gaddr & 0xFFU); /* patch gptr, LE */
            block[145] = (uint8_t)((gaddr >> 8) & 0xFFU);
            block[146] = (uint8_t)((gaddr >> 16) & 0xFFU);
            block[147] = (uint8_t)((gaddr >> 24) & 0xFFU);
            EVE_memWrite_flash_buffer(maddr, block, 148UL);
        }
        else
        {
            Serial.printf("Font %u inflate FAILED twice -> ROM font 31 fallback\r\n", (unsigned)i);
            all_ok = false;
        }

        g_fonts[i].metrics_addr = maddr;
        g_fonts[i].ok = ok;
        addr = (gend_expected + 3UL) & ~3UL;
    }

    Serial.printf("RAM_G: fonts %lu bytes (headroom %lu)\r\n",
                  (unsigned long)addr, (unsigned long)(EVE_RAM_G_SIZE - addr));
    return all_ok;
}

/* Render-time font selector: the instance's bitmap handle, or ROM font 31
 * when that instance failed to load. */
uint16_t dash_font(uint8_t idx)
{
    return g_fonts[idx].ok ? (uint16_t)DASH_FONTS[idx].handle : 31U;
}

/* CMD_SETFONT2 emits display-list commands, so every frame that uses the
 * custom fonts re-registers them at its top (~5 words per instance). */
void dash_register_fonts(void)
{
    for (uint8_t i = 0U; i < DASH_FONT_COUNT; i++)
    {
        if (g_fonts[i].ok)
        {
            EVE_cmd_setfont2((uint32_t)DASH_FONTS[i].handle,
                             g_fonts[i].metrics_addr,
                             (uint32_t)DASH_FONTS[i].firstchar);
        }
    }
}

/* ---- odometer EEPROM glue (KTD7): the pure record logic lives in
 * dash_odo.h; this is the only code that touches the EEPROM API ---- */
void odo_eeprom_load(void)
{
    /* Two-slot ping-pong (review finding): a torn write -- power loss
     * mid-write on the 12V rail -- corrupts at most the slot being
     * written; the other slot still holds the previous odometer, so a
     * tear costs 0.1 mi, never the lifetime count. */
    uint8_t rec0[DASH_ODO_RECORD_SIZE];
    uint8_t rec1[DASH_ODO_RECORD_SIZE];
    eeprom_read_block(rec0, (const void *)DASH_ODO_SLOT_ADDR(0), DASH_ODO_RECORD_SIZE);
    eeprom_read_block(rec1, (const void *)DASH_ODO_SLOT_ADDR(1), DASH_ODO_RECORD_SIZE);
    if (dash_odo_pick_load_slot(rec0, rec1, &g_odo) == 0xFFU)
    {
        dash_odo_init(&g_odo); /* blank or corrupt EEPROM: clean zero start */
    }
}

void odo_eeprom_write(void)
{
    uint8_t rec[DASH_ODO_RECORD_SIZE];
    uint8_t slot;
    dash_odo_encode_next(&g_odo, rec, &slot); /* bumps seq, alternates slots */
    eeprom_write_block(rec, (void *)DASH_ODO_SLOT_ADDR(slot), DASH_ODO_RECORD_SIZE);
}

/* ---- serial pump (KTD6): accumulate a line, parse, apply, ack ---- */
void pump_serial(void)
{
    while (Serial.available() > 0)
    {
        const char c = (char)Serial.read();
        if ('\n' == c)
        {
            g_serial_line[g_serial_len] = '\0';
            handle_serial_line(g_serial_line);
            g_serial_len = 0U;
        }
        else if (g_serial_len < (sizeof(g_serial_line) - 1U))
        {
            g_serial_line[g_serial_len++] = c;
        }
    }
}

void handle_serial_line(const char *line)
{
    DashCommand cmd;
    const DashSerialErr err = dash_parse_line(line, &cmd);

    if (DASH_ERR_EMPTY == err)
    {
        return; /* blank lines are ignored silently */
    }
    if (DASH_ERR_NONE != err)
    {
        static const char *const reasons[] = {
            "none", "empty", "unknown command", "unknown channel",
            "missing value", "bad value", "value out of range", "line too long"
        };
        Serial.printf("err %s\r\n", reasons[err]);
        return;
    }

    char reply[96];
    if (dash_apply_command(&g_dash, &cmd, reply, sizeof(reply)))
    {
        Serial.println(reply);
        return;
    }

    /* ODO_SET / STATUS / HELP are the caller's (ours) to handle */
    switch (cmd.kind)
    {
        case DASH_CMD_ODO_SET:
            dash_odo_reseed(&g_odo, cmd.value);
            odo_eeprom_write();
            dash_odo_mark_written(&g_odo);
            Serial.printf("ok odo set %.1f\r\n", (double)dash_odo_miles(&g_odo));
            break;
        case DASH_CMD_STATUS: {
            /* Full-state snapshot (context parity, review finding): every
             * channel with `--` for invalid -- matching what the panel
             * dead-fronts -- plus the active alarm, sim state, and the
             * auto-recovered coprocessor fault count. One line, one ack. */
            const DashAlarm alarm = dash_alarm_classify(&g_dash);
            Serial.printf("ok status mode=%s fps=%u alarm=%s sim=%s faults=%lu",
                          (DASH_MODE_TRACK == g_dash.mode) ? "track" : "street",
                          (unsigned)g_fps,
                          (DASH_ALARM_OILP == alarm) ? "oilp" :
                          (DASH_ALARM_OILT == alarm) ? "oilt" :
                          (DASH_ALARM_CLT == alarm) ? "clt" : "none",
                          g_dash.sim_frozen ? "off" : "on",
                          (unsigned long)g_eve_faults);
            for (uint8_t ch = 0U; ch < DASH_CH_COUNT; ch++)
            {
                if (dash_ch_valid(&g_dash, ch))
                {
                    Serial.printf(" %s=%g", dash_ch_name(ch), (double)dash_ch_get(&g_dash, ch));
                }
                else
                {
                    Serial.printf(" %s=--", dash_ch_name(ch));
                }
            }
            Serial.printf(" odo=%.1f trip=%.1f dl=%u/%u eve=%s\r\n",
                          (double)dash_odo_miles(&g_odo), (double)dash_trip_miles(&g_odo),
                          (unsigned)g_dl_track, (unsigned)g_dl_street,
                          eve_ready ? "ok" : "INIT-FAILED");
            break;
        }
        case DASH_CMD_HELP:
            Serial.printf("ok %s\r\n", DASH_HELP_TEXT);
            break;
        default:
            Serial.println(F("err unhandled command"));
            break;
    }
}
