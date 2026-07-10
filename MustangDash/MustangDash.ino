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

/* ---- asset plumbing ---- */

/* Splash assets are ASTC bitmaps resident in the panel's 64 MB QSPI flash
 * (address table + provisioning pack in splash_flash.h) and are rendered
 * directly from flash. RAM_G is untouched by the splash -- the dash fonts
 * below are its only tenant. */

struct ThemeDesc
{
    const SplashFlashAsset *bg;    /* 1024x600 ASTC 8x8, drawn native 1:1 */
    const SplashFlashAsset *side;  /* chrome bars or checker blocks, drawn left+right */
    const SplashFlashAsset *line;  /* accent line, revealed from center */
    const SplashFlashAsset *year;  /* "1965" */
    const SplashFlashAsset *strip; /* checkered only: full-width edge strip */
    bool has_strip;
    int16_t side_y; /* bars sit at y 202, checker blocks at y 198 */
    int16_t line_y; /* line canvas at y 420 (blue/red) or y 440 (checkered) */
};

#define SPLASH_FA(idx) (&SPLASH_FLASH_ASSETS[idx])

static const ThemeDesc THEMES[3] = {
    /* SPLASH_THEME_BLUE */
    { SPLASH_FA(SPLASH_FA_BG_BLUE), SPLASH_FA(SPLASH_FA_BARS_CHROME),
      SPLASH_FA(SPLASH_FA_LINE_BLUE), SPLASH_FA(SPLASH_FA_YEAR_BLUE),
      nullptr, false, SPLASH_BAR_Y, SPLASH_LINE_Y },
    /* SPLASH_THEME_RED */
    { SPLASH_FA(SPLASH_FA_BG_RED), SPLASH_FA(SPLASH_FA_BARS_CHROME),
      SPLASH_FA(SPLASH_FA_LINE_RED), SPLASH_FA(SPLASH_FA_YEAR_RED),
      nullptr, false, SPLASH_BAR_Y, SPLASH_LINE_Y },
    /* SPLASH_THEME_CHECKERED */
    { SPLASH_FA(SPLASH_FA_BG_CHECKERED), SPLASH_FA(SPLASH_FA_CHECKER_BLOCK),
      SPLASH_FA(SPLASH_FA_CHECKER_LINE), SPLASH_FA(SPLASH_FA_YEAR_CHECKERED),
      SPLASH_FA(SPLASH_FA_CHECKER_STRIP), true,
      SPLASH_CBLOCK_Y, SPLASH_CLINE_Y },
};

/* THEMES[] rows are positional: the theme id doubles as the array index. */
static_assert((SPLASH_THEME_BLUE == 0) && (SPLASH_THEME_RED == 1) && (SPLASH_THEME_CHECKERED == 2),
              "THEMES[] rows are ordered blue, red, checkered and indexed by the SPLASH_THEME_* values");

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

/* One sweep gauge (KTD2/KTD9): geometry derives from the design's shared
 * viewBox fractions (ticks r80-88, labels r66, needle r62, hub r7). */
struct GaugeSpec
{
    float cx, cy, r; /* panel px */
    int16_t stroke;  /* arc stroke, panel px */
};

struct Telltale
{
    const char *label;
    bool valid;
    bool lit;
    uint32_t color;
};

static char g_serial_line[DASH_SERIAL_MAX_LINE + 17]; /* headroom to detect too-long */
static uint8_t g_serial_len = 0U;

static uint32_t g_loop_last_ms = 0UL;
static uint16_t g_fps = 0U;         /* frames completed in the last full second */
static uint16_t g_fps_frames = 0U;
static uint32_t g_fps_window_ms = 0UL;
static uint16_t g_dl_track = 0U;    /* boot-measured DL usage per mode (bytes/4) */
static uint16_t g_dl_street = 0U;
static uint32_t g_eve_faults = 0UL; /* coprocessor faults auto-recovered by EVE_busy() */

/* RAM_G scratch used only while provisioning the splash pack at boot:
 * a 64-byte header readback at the top of RAM_G and a 32 KB staging
 * buffer for CMD_FLASHUPDATE chunks just below it -- both far above the
 * fonts (~273 KB from address 0). */
static const uint32_t SCRATCH_HDR = 0xFF000UL;
static const uint32_t SCRATCH_BUF = 0xF7000UL;
static const uint32_t PROVISION_CHUNK = 0x8000UL; /* 32 KB, multiple of 4096 */

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

static const uint32_t CROSSFADE_MS = 400UL;

static bool eve_ready = false; /* set true only if EVE_init() reported E_OK */

/* ---- forward declarations (explicit prototypes, see note above) ---- */
void set_backlight(uint8_t duty);
void eve_frame_begin(uint32_t clear_rgb);
void eve_frame_end(void);
bool splash_header_current(void);
bool splash_flash_provision(void);
void draw_flash_asset(const SplashFlashAsset *a, int16_t x, int16_t y);
void draw_splash_background(const SplashFlashAsset *bg, uint8_t alpha);
void draw_splash_emblem(const SplashFlashAsset *emblem, float scale, uint8_t alpha);
void draw_splash_elements(const ThemeDesc *theme, uint32_t now_ms, uint8_t global_alpha);
void run_splash(const ThemeDesc *theme);
bool load_dash_fonts(void);
uint16_t dash_font(uint8_t idx);
void dash_register_fonts(void);
void odo_eeprom_load(void);
void odo_eeprom_write(void);
void pump_serial(void);
void handle_serial_line(const char *line);
void dash_color(uint32_t rgb, uint8_t alpha);
void draw_dash_content(uint32_t now_ms, uint8_t alpha);
void draw_track_mode(uint32_t now_ms, uint8_t alpha);
void draw_street_mode(uint32_t now_ms, uint8_t alpha);
void draw_alarm_takeover(DashAlarm alarm, uint32_t now_ms, uint8_t alpha);
uint16_t measure_mode_dl(DashMode mode);
void dash_frame(uint32_t now_ms);

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

/* Read the 64-byte pack header from flash and compare it against the
 * embedded pack's identity. CMD_FLASHREAD wants src 64-byte aligned, dest
 * 4-byte aligned, length a multiple of 4 (guide 5.80) -- all satisfied. */
bool splash_header_current(void)
{
    EVE_cmd_flashread(SCRATCH_HDR, SPLASH_FLASH_BASE, 64UL);
    const uint32_t magic = EVE_memRead32(SCRATCH_HDR);
    const uint32_t version = EVE_memRead32(SCRATCH_HDR + 4UL);
    const uint32_t length = EVE_memRead32(SCRATCH_HDR + 8UL);
    const uint32_t count = EVE_memRead32(SCRATCH_HDR + 12UL);
    const uint32_t crc = EVE_memRead32(SCRATCH_HDR + 16UL);
    return (SPLASH_FLASH_MAGIC == magic) && (SPLASH_FLASH_VERSION == version) &&
           (SPLASH_FLASH_PACK_SIZE == length) && ((uint32_t)SPLASH_FA_COUNT == count) &&
           (SPLASH_FLASH_CRC == crc);
}

/* Make sure the splash pack in the panel's QSPI flash matches the embedded
 * one; provision it if not (first boot, or the generated assets changed).
 *
 * The pack starts at flash address 4096 and offsets only grow, so sector 0
 * (the vendor-programmed flashfast BLOB, flash 0..4095) is never written.
 * CMD_FLASHUPDATE runs fine in the full-speed state EVE_init_flash() left
 * us in (guide 5.82 / Table 5; it only erases+writes 4 KB sectors whose
 * content differs), with dest 4096-aligned, src 4-aligned and length a
 * multiple of 4096 -- our 32 KB chunks satisfy all three.
 * Returns true when the flash assets are known-good. */
bool splash_flash_provision(void)
{
    if (splash_header_current())
    {
        Serial.printf("Splash flash assets current (v%lu, crc 0x%08lX OK)\r\n",
                      (unsigned long)SPLASH_FLASH_VERSION,
                      (unsigned long)SPLASH_FLASH_CRC);
        return true;
    }

    Serial.printf("Splash flash assets stale/missing -> provisioning %lu KB ",
                  (unsigned long)(SPLASH_FLASH_PACK_SIZE / 1024UL));
    /* The 64-byte header lives in chunk 0, and splash_header_current() only
     * ever reads the header back -- so the header chunk is the COMMIT
     * RECORD and must be written LAST. Writing it first would let a
     * power-cut provisioning run (header ok, later chunks never written)
     * pass verification on every subsequent boot and render garbage
     * assets forever (review finding: torn provisioning). */
    for (uint32_t off = PROVISION_CHUNK; off < SPLASH_FLASH_PACK_SIZE; off += PROVISION_CHUNK)
    {
        const uint32_t n = min(PROVISION_CHUNK, SPLASH_FLASH_PACK_SIZE - off);
        EVE_memWrite_flash_buffer(SCRATCH_BUF, &splash_flash_pack[off], n);
        EVE_cmd_flashupdate(SPLASH_FLASH_BASE + off, SCRATCH_BUF, n);
        Serial.print('.');
    }
    EVE_memWrite_flash_buffer(SCRATCH_BUF, &splash_flash_pack[0],
                              min(PROVISION_CHUNK, SPLASH_FLASH_PACK_SIZE));
    EVE_cmd_flashupdate(SPLASH_FLASH_BASE, SCRATCH_BUF,
                        min(PROVISION_CHUNK, SPLASH_FLASH_PACK_SIZE));
    Serial.print('.');
    Serial.println();

    /* flash contents changed: clear the graphics engine's bitmap cache so
     * no stale blocks render (guide 5.89; the library helper swaps in the
     * empty display lists CMD_CLEARCACHE requires first) */
    EVE_cmd_clearcache();

    if (splash_header_current())
    {
        Serial.println(F("Provisioning done, header verified."));
        return true;
    }
    Serial.println(F("Provisioning FAILED verification -> skipping splash."));
    return false;
}

/* Draw a flash-resident ASTC bitmap 1:1 at (x, y) in pixels. CMD_SETBITMAP
 * takes extended (>17) formats and emits BITMAP_EXT_FORMAT plus the
 * LAYOUT(_H)/SIZE(_H) words itself (guide 5.75); a flash source is bit 23
 * set plus the address in 32-byte blocks. Filter comes out NEAREST, which
 * is exact for 1:1 draws. */
void draw_flash_asset(const SplashFlashAsset *a, int16_t x, int16_t y)
{
    EVE_cmd_setbitmap(0x800000UL | (a->addr / 32UL), (uint16_t)a->fmt, a->w, a->h);
    EVE_cmd_dl(DL_BEGIN | EVE_BITMAPS);
    EVE_cmd_dl(VERTEX2F((int16_t)(x * 16), (int16_t)(y * 16)));
    EVE_cmd_dl(DL_END);
}

/* Full-screen background: native 1024x600 ASTC 8x8 straight from flash,
 * drawn 1:1 at the origin -- no scale transform needed anymore. */
void draw_splash_background(const SplashFlashAsset *bg, uint8_t alpha)
{
    EVE_cmd_dl(COLOR_A(alpha));
    draw_flash_asset(bg, 0, 0);
}

/* Emblem with scale-about-center: drawn in a 220 px window so the ease-out
 * back overshoot (peak scale ~1.03 -> 206 px) never clips at the edges. The
 * window is anchored 10 px up-left of the emblem's final position and the
 * transform pins the bitmap's center to the window's center. */
void draw_splash_emblem(const SplashFlashAsset *emblem, float scale, uint8_t alpha)
{
    const int16_t win = SPLASH_EMBLEM_DRAW_PX;                    /* 220 */
    const int16_t margin = (int16_t)((win - emblem->w) / 2);      /* 10 */
    const int16_t win_x = (int16_t)(SPLASH_EMBLEM_X - margin);    /* 402 */
    const int16_t win_y = (int16_t)(SPLASH_EMBLEM_Y - margin);    /* 114 */
    const long half_win = (long)(win / 2) * 65536L;               /* 110 in 16.16 */
    const long half_bmp_x = (long)(emblem->w / 2) * 65536L;       /* 100 in 16.16 */
    const long half_bmp_y = (long)(emblem->h / 2) * 65536L;

    EVE_cmd_dl(COLOR_A(alpha));
    EVE_cmd_setbitmap(0x800000UL | (emblem->addr / 32UL), (uint16_t)emblem->fmt,
                      emblem->w, emblem->h);
    /* re-emit SIZE: SETBITMAP defaults to NEAREST and the bitmap's own
     * dimensions; the scaled emblem needs BILINEAR in a 220 px window */
    EVE_cmd_dl(BITMAP_SIZE(EVE_BILINEAR, EVE_BORDER, EVE_BORDER,
                           (uint16_t)win, (uint16_t)win));
    EVE_cmd_dl(CMD_LOADIDENTITY);
    EVE_cmd_translate(half_win, half_win); /* window center... */
    EVE_cmd_scale((long)(scale * 65536.0f), (long)(scale * 65536.0f));
    EVE_cmd_translate(-half_bmp_x, -half_bmp_y); /* ...pins bitmap center */
    EVE_cmd_dl(CMD_SETMATRIX);
    EVE_cmd_dl(DL_BEGIN | EVE_BITMAPS);
    EVE_cmd_dl(VERTEX2F((int16_t)(win_x * 16), (int16_t)(win_y * 16)));
    EVE_cmd_dl(DL_END);
    EVE_cmd_dl(CMD_LOADIDENTITY);
    EVE_cmd_dl(CMD_SETMATRIX);
}

/* One splash composition at time now_ms. global_alpha scales every element
 * (255 during the animation; ramps down during the crossfade). */
void draw_splash_elements(const ThemeDesc *theme, uint32_t now_ms, uint8_t global_alpha)
{
    /* alpha helper: element alpha scaled by the crossfade's global alpha */
#define SPLASH_A(elem_a) ((uint8_t)(((uint16_t)(elem_a) * (uint16_t)global_alpha) / 255U))

    EVE_color_rgb(0xFFFFFFUL); /* draw bitmaps untinted */

    draw_splash_background(theme->bg, global_alpha);

    /* bars / checker blocks slide in from off-screen, fading up */
    const float bars_eased = splash_bars_eased(now_ms);
    const uint8_t bars_a = SPLASH_A(splash_bars_alpha(now_ms));
    if (bars_a > 0U)
    {
        EVE_cmd_dl(COLOR_A(bars_a));
        draw_flash_asset(theme->side, splash_bar_left_x(now_ms), theme->side_y);
        draw_flash_asset(theme->side, splash_bar_right_x(now_ms), theme->side_y);

        if (theme->has_strip)
        {
            /* edge strips ride the bars' timing: top with the left bar's
             * travel, bottom with the right's; the bottom starts 13 px
             * offset for an alternating pattern, drawn twice to wrap the
             * sliver at the right edge */
            const int16_t travel = (int16_t)(288.0f * (1.0f - bars_eased));
            const SplashFlashAsset *strip = theme->strip;
            draw_flash_asset(strip, (int16_t)(0 - travel), SPLASH_CSTRIP_TOP_Y);
            const int16_t bot_x = (int16_t)(-SPLASH_CSTRIP_ALT_OFFSET + travel);
            draw_flash_asset(strip, bot_x, SPLASH_CSTRIP_BOT_Y);
            /* wraparound copy for the right-edge sliver -- drawn only once it
             * starts on-screen: past EVE_HSIZE the x*16 value exceeds
             * VERTEX2F's signed 15-bit field and would wrap to the left side
             * of the panel mid-slide, defeating the slide-in */
            const int16_t wrap_x = (int16_t)(bot_x + (int16_t)strip->w);
            if (wrap_x < (int16_t)EVE_HSIZE)
            {
                draw_flash_asset(strip, wrap_x, SPLASH_CSTRIP_BOT_Y);
            }
        }
    }

    /* emblem pops with overshoot */
    const uint8_t emblem_a = SPLASH_A(splash_emblem_alpha(now_ms));
    if (emblem_a > 0U)
    {
        draw_splash_emblem(SPLASH_FA(SPLASH_FA_EMBLEM), splash_emblem_scale(now_ms), emblem_a);
    }

    /* wordmark + year rise together */
    const uint8_t word_a = SPLASH_A(splash_word_alpha(now_ms));
    if (word_a > 0U)
    {
        const int16_t dy = splash_word_dy(now_ms);
        EVE_cmd_dl(COLOR_A(word_a));
        draw_flash_asset(SPLASH_FA(SPLASH_FA_WORDMARK), SPLASH_WORD_X, (int16_t)(SPLASH_WORD_Y + dy));
        draw_flash_asset(theme->year, SPLASH_YEAR_X, (int16_t)(SPLASH_YEAR_Y + dy));
    }

    /* accent line sweeps open from center, behind an expanding scissor */
    const float reveal = splash_line_reveal(now_ms);
    if (reveal > 0.0f)
    {
        const SplashFlashAsset *line = theme->line;
        const uint16_t reveal_w = (uint16_t)((float)line->w * reveal + 0.5f);
        if (reveal_w > 0U)
        {
            const int16_t line_x = (int16_t)((EVE_HSIZE / 2U) - (line->w / 2U));
            EVE_cmd_dl(COLOR_A(global_alpha));
            EVE_cmd_dl(DL_SAVE_CONTEXT);
            EVE_cmd_dl(SCISSOR_XY((uint16_t)((EVE_HSIZE / 2U) - (reveal_w / 2U)),
                                  (uint16_t)theme->line_y));
            EVE_cmd_dl(SCISSOR_SIZE(reveal_w, line->h));
            draw_flash_asset(line, line_x, theme->line_y);
            EVE_cmd_dl(DL_RESTORE_CONTEXT);
        }
    }

    EVE_cmd_dl(COLOR_A(255U));
#undef SPLASH_A
}

/* ---- dash rendering ----
 *
 * Everything below draws in plain (non-burst) command mode: at 8 MHz SPI a
 * full STREET frame is a few ms of transfer, comfortably over 30 fps, and
 * plain mode lets the crossfade mix splash + dash in one display list. The
 * _burst path (KTD8) stays available as an optimization if the measured fps
 * ever drops.
 *
 * Layout: mock coordinates (620x400) scaled through DASH_LX/LY (positions,
 * rect extents) and DASH_LR (radii, strokes, font-ish sizes) per KTD9.
 * Invalid channels follow the KTD4 convention: text renders "--", fills
 * render empty, LEDs dark, needles park at rest at dead-front opacity,
 * telltales show the distinct COLOR_NODATA state. */

/* font registry indices (order fixed by tools/make_dash_fonts.py) */
enum
{
    DF_HERO = 0, DF_BIG, DF_MID, DF_VAL, DF_SMALL, DF_TITLE, DF_LABEL, DF_TINY
};

/* COLOR_RGB + COLOR_A in one call; alpha pre-scaled by the crossfade. */
void dash_color(uint32_t rgb, uint8_t alpha)
{
    EVE_color_rgb(rgb);
    EVE_cmd_dl(COLOR_A(alpha));
}

/* Text color for an RPM-driven readout (shared by the TRACK value and the
 * STREET tach hub). */
static uint32_t dash_state_text_color(DashColorState rc)
{
    return (DASH_COLOR_RED == rc) ? COLOR_RED_TEXT :
           (DASH_COLOR_AMBER == rc) ? COLOR_AMBER : COLOR_VALUE;
}

#define DA(a) ((uint8_t)(((uint16_t)(a) * (uint16_t)alpha) / 255U))

/* A rounded-capsule bar: RECTS with LINE_WIDTH as the corner radius. */
static void draw_pill(int16_t x, int16_t y, int16_t w, int16_t h)
{
    const int16_t r = (int16_t)(h / 2);
    EVE_cmd_dl(DL_LINE_WIDTH | (uint32_t)(r * 16));
    EVE_cmd_dl(DL_BEGIN | EVE_RECTS);
    EVE_cmd_dl(VERTEX2F((int16_t)((x + r) * 16), (int16_t)((y + r) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)((x + w - r) * 16), (int16_t)((y + h - r) * 16)));
    EVE_cmd_dl(DL_END);
}

/* Value arc as a LINE_STRIP polyline, one segment per ~3 degrees, endpoints
 * precomputed on the M7 (no CMD_ARC on BT817). frac0 < frac1 in gauge
 * fractions; stroke_px is the full stroke width. */
static void draw_arc(float cx, float cy, float r, float frac0, float frac1, int16_t stroke_px)
{
    const float sweep = frac1 - frac0;
    if (sweep <= 0.0f)
    {
        return;
    }
    int n = (int)(sweep * 80.0f) + 1; /* 80 segments per full 240 deg sweep */
    if (n < 2) { n = 2; }

    EVE_cmd_dl(DL_LINE_WIDTH | (uint32_t)(stroke_px * 8)); /* half-width in 1/16 px */
    EVE_cmd_dl(DL_BEGIN | EVE_LINE_STRIP);
    for (int i = 0; i <= n; i++)
    {
        float x;
        float y;
        dash_arc_point(cx, cy, r, frac0 + sweep * ((float)i / (float)n), &x, &y);
        EVE_cmd_dl(VERTEX2F((int16_t)(x * 16.0f), (int16_t)(y * 16.0f)));
    }
    EVE_cmd_dl(DL_END);
}

static void draw_gauge_chrome(const GaugeSpec *g, uint8_t alpha)
{
    dash_color(COLOR_ARC_TRACK, alpha);
    draw_arc(g->cx, g->cy, g->r, 0.0f, 1.0f, g->stroke);
}

static void draw_gauge_needle_hub(const GaugeSpec *g, float frac, bool valid, uint8_t alpha)
{
    const uint8_t na = valid ? alpha : DA(70); /* dead-front park when invalid */
    float nx;
    float ny;
    dash_arc_point(g->cx, g->cy, g->r * DASH_GAUGE_NEEDLE_R_FRAC,
                   valid ? frac : 0.0f, &nx, &ny);

    dash_color(COLOR_VALUE, na);
    EVE_cmd_dl(DL_LINE_WIDTH | (uint32_t)(DASH_LR(3) * 8));
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(g->cx * 16.0f), (int16_t)(g->cy * 16.0f)));
    EVE_cmd_dl(VERTEX2F((int16_t)(nx * 16.0f), (int16_t)(ny * 16.0f)));
    EVE_cmd_dl(DL_END);

    /* hub disc + ring */
    const int16_t hub_r = (int16_t)(g->r * DASH_GAUGE_HUB_R_FRAC);
    dash_color(COLOR_HUB_RING, alpha);
    EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)((hub_r + DASH_LR(2)) * 16));
    EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
    EVE_cmd_dl(VERTEX2F((int16_t)(g->cx * 16.0f), (int16_t)(g->cy * 16.0f)));
    EVE_cmd_dl(DL_END);
    dash_color(COLOR_BAR_TRACK, alpha);
    EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)(hub_r * 16));
    EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
    EVE_cmd_dl(VERTEX2F((int16_t)(g->cx * 16.0f), (int16_t)(g->cy * 16.0f)));
    EVE_cmd_dl(DL_END);
}

static void draw_gauge_ticks(const GaugeSpec *g, const float *fracs, uint8_t count, uint8_t alpha)
{
    dash_color(COLOR_HUB_RING, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | (uint32_t)(DASH_LR(2) * 8));
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    for (uint8_t i = 0U; i < count; i++)
    {
        float x0, y0, x1, y1;
        dash_arc_point(g->cx, g->cy, g->r * DASH_GAUGE_TICK_IN_FRAC, fracs[i], &x0, &y0);
        dash_arc_point(g->cx, g->cy, g->r * DASH_GAUGE_TICK_OUT_FRAC, fracs[i], &x1, &y1);
        EVE_cmd_dl(VERTEX2F((int16_t)(x0 * 16.0f), (int16_t)(y0 * 16.0f)));
        EVE_cmd_dl(VERTEX2F((int16_t)(x1 * 16.0f), (int16_t)(y1 * 16.0f)));
    }
    EVE_cmd_dl(DL_END);
}

/* ---- TRACK mode (U7): shift lights, GPS hero, RPM bar, lap/delta ---- */
void draw_track_mode(uint32_t now_ms, uint8_t alpha)
{
    char buf[24];
    const bool rpm_ok = dash_ch_valid(&g_dash, DASH_CH_RPM);
    const bool spd_ok = dash_ch_valid(&g_dash, DASH_CH_SPEED);
    const float rpm = g_dash.ch.rpm;

    /* shift lights: 15 LEDs, dia 18 mock, gap 9; all dark when rpm invalid */
    const bool flash_zone = rpm_ok && dash_shift_flash_zone(rpm);
    const bool flash_on = dash_flash_phase(now_ms, DASH_SHIFT_FLASH_HALF_MS);
    const uint8_t lit = rpm_ok ? dash_shift_led_count(rpm) : 0U;
    const int16_t led_r = (int16_t)(DASH_LR(18) / 2);
    for (uint8_t i = 0U; i < DASH_SHIFT_LED_COUNT; i++)
    {
        const int16_t cx = DASH_LX(121 + i * 27);
        const int16_t cy = DASH_LY(31);
        bool on = rpm_ok && ((i + 1U) <= lit);
        uint32_t col = (i < 10U) ? COLOR_GREEN : (i < 13U) ? COLOR_AMBER : COLOR_RED_FILL;
        if (flash_zone)
        {
            on = flash_on;
            col = COLOR_RED_FILL;
        }
        if (on)
        {
            /* glow: a larger soft point behind the lit LED */
            dash_color(col, DA(60));
            EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)((led_r + DASH_LR(6)) * 16));
            EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
            EVE_cmd_dl(VERTEX2F((int16_t)(cx * 16), (int16_t)(cy * 16)));
            EVE_cmd_dl(DL_END);
        }
        dash_color(on ? col : COLOR_DEAD_DOT, on ? alpha : DA(230));
        EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)(led_r * 16));
        EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
        EVE_cmd_dl(VERTEX2F((int16_t)(cx * 16), (int16_t)(cy * 16)));
        EVE_cmd_dl(DL_END);
    }

    /* hairline divider under the LED row */
    dash_color(COLOR_HAIRLINE, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(20) * 16), (int16_t)(DASH_LY(60) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(600) * 16), (int16_t)(DASH_LY(60) * 16)));
    EVE_cmd_dl(DL_END);

    /* GPS SPEED hero, centered in the left column (mock center x 210) */
    const int16_t hx = DASH_LX(210);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(hx, DASH_LY(112), dash_font(DF_LABEL), EVE_OPT_CENTERX, "GPS SPEED");
    dash_color(COLOR_VALUE, alpha);
    if (spd_ok)
    {
        snprintf(buf, sizeof(buf), "%d", (int)(g_dash.ch.speed_mph + 0.5f));
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text(hx, DASH_LY(185), dash_font(DF_HERO), EVE_OPT_CENTER, buf);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(hx, DASH_LY(250), dash_font(DF_LABEL), EVE_OPT_CENTERX, "MPH");

    /* RPM row: label, live value, capsule bar, redline marker, scale */
    const int16_t bar_x = DASH_LX(37);
    const int16_t bar_w = (int16_t)(DASH_LX(383) - bar_x);
    const int16_t bar_y = DASH_LY(285);
    const int16_t bar_h = DASH_LY(8);

    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(bar_x, DASH_LY(262), dash_font(DF_LABEL), 0U, "RPM");
    const DashColorState rc = rpm_ok ? dash_rpm_color(rpm) : DASH_COLOR_NORMAL;
    dash_color(dash_state_text_color(rc), alpha);
    if (rpm_ok)
    {
        snprintf(buf, sizeof(buf), "%d", (int)(rpm + 0.5f));
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text((int16_t)(bar_x + bar_w), DASH_LY(258), dash_font(DF_VAL), EVE_OPT_RIGHTX, buf);

    dash_color(COLOR_BAR_TRACK, alpha);
    draw_pill(bar_x, bar_y, bar_w, bar_h);
    if (rpm_ok && (rpm > 0.0f))
    {
        const float bfrac = (rpm >= (float)DASH_REDLINE_RPM) ? 1.0f : (rpm / (float)DASH_REDLINE_RPM);
        const int16_t fw = (int16_t)((float)bar_w * bfrac);
        if (fw > bar_h)
        {
            dash_color((DASH_COLOR_RED == rc) ? COLOR_RED_FILL : COLOR_ACCENT, alpha);
            draw_pill(bar_x, bar_y, fw, bar_h);
        }
    }
    /* redline marker at 96% of the bar, 2 px, half opacity */
    dash_color(COLOR_RED_FILL, DA(128));
    EVE_cmd_dl(DL_LINE_WIDTH | 16UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    const int16_t red_x = (int16_t)(bar_x + (int16_t)((float)bar_w * 0.96f));
    EVE_cmd_dl(VERTEX2F((int16_t)(red_x * 16), (int16_t)(bar_y * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(red_x * 16), (int16_t)((bar_y + bar_h) * 16)));
    EVE_cmd_dl(DL_END);

    for (uint8_t i = 0U; i < 5U; i++)
    {
        dash_color((4U == i) ? COLOR_MUTED_RED : COLOR_FAINT, alpha);
        const int16_t sx = (int16_t)(bar_x + (int32_t)bar_w * i / 4);
        snprintf(buf, sizeof(buf), "%u", (unsigned)(i * 2U));
        EVE_cmd_text(sx, DASH_LY(300), dash_font(DF_TINY), EVE_OPT_CENTERX, buf);
    }

    /* right column: hairline, LAP TIME, DELTA value + center-zero bar */
    dash_color(COLOR_HAIRLINE, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(412) * 16), (int16_t)(DASH_LY(100) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(412) * 16), (int16_t)(DASH_LY(300) * 16)));
    EVE_cmd_dl(DL_END);

    const int16_t col_x = DASH_LX(428);
    const int16_t col_x1 = DASH_LX(582);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(col_x, DASH_LY(140), dash_font(DF_LABEL), 0U, "LAP TIME");
    dash_color(COLOR_VALUE, alpha);
    dash_fmt_lap(g_dash.ch.lap_ms, dash_ch_valid(&g_dash, DASH_CH_LAP), buf);
    EVE_cmd_text(col_x, DASH_LY(158), dash_font(DF_MID), 0U, buf);

    const bool delta_ok = dash_ch_valid(&g_dash, DASH_CH_DELTA);
    const float dfill = delta_ok ? dash_delta_fill(g_dash.ch.delta_s) : 0.0f;
    const uint32_t dcol = (dfill <= 0.0f) ? COLOR_GREEN : COLOR_RED_TEXT;
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(col_x, DASH_LY(212), dash_font(DF_LABEL), 0U, "DELTA");
    dash_color(delta_ok ? dcol : COLOR_VALUE, alpha);
    if (delta_ok)
    {
        snprintf(buf, sizeof(buf), "%+.2f", (double)g_dash.ch.delta_s);
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text(col_x1, DASH_LY(208), dash_font(DF_VAL), EVE_OPT_RIGHTX, buf);

    const int16_t db_x = col_x;
    const int16_t db_w = (int16_t)(col_x1 - col_x);
    const int16_t db_y = DASH_LY(232);
    const int16_t db_h = DASH_LY(8);
    const int16_t db_cx = (int16_t)(db_x + db_w / 2);
    dash_color(COLOR_BAR_TRACK, alpha);
    draw_pill(db_x, db_y, db_w, db_h);
    dash_color(COLOR_HUB_RING, alpha); /* center-zero marker */
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(db_cx * 16), (int16_t)(db_y * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(db_cx * 16), (int16_t)((db_y + db_h) * 16)));
    EVE_cmd_dl(DL_END);
    if (delta_ok && (dfill != 0.0f))
    {
        const int16_t fw = (int16_t)((float)(db_w / 2) * ((dfill < 0.0f) ? -dfill : dfill));
        if (fw > 1)
        {
            dash_color(dcol, alpha);
            EVE_cmd_dl(DL_BEGIN | EVE_RECTS);
            EVE_cmd_dl(DL_LINE_WIDTH | 16UL);
            if (dfill < 0.0f) /* ahead: fill left of center */
            {
                EVE_cmd_dl(VERTEX2F((int16_t)((db_cx - fw) * 16), (int16_t)(db_y * 16)));
                EVE_cmd_dl(VERTEX2F((int16_t)(db_cx * 16), (int16_t)((db_y + db_h) * 16)));
            }
            else /* behind: fill right of center */
            {
                EVE_cmd_dl(VERTEX2F((int16_t)(db_cx * 16), (int16_t)(db_y * 16)));
                EVE_cmd_dl(VERTEX2F((int16_t)((db_cx + fw) * 16), (int16_t)((db_y + db_h) * 16)));
            }
            EVE_cmd_dl(DL_END);
        }
    }

    /* footer: odometer (label left, value right) */
    dash_color(COLOR_HAIRLINE, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(22) * 16), (int16_t)(DASH_LY(358) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(598) * 16), (int16_t)(DASH_LY(358) * 16)));
    EVE_cmd_dl(DL_END);
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH_LX(22), DASH_LY(368), dash_font(DF_TINY), 0U, "ODOMETER");
    dash_color(COLOR_ODO, alpha);
    snprintf(buf, sizeof(buf), "%.1f", (double)dash_odo_miles(&g_odo));
    EVE_cmd_text(DASH_LX(570), DASH_LY(365), dash_font(DF_SMALL), EVE_OPT_RIGHTX, buf);
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH_LX(578), DASH_LY(368), dash_font(DF_TINY), 0U, "MI");
}

/* ---- STREET mode (U8): dual sweep gauges, telltales, odometer ---- */

/* speedo tick fractions: every 20 MPH through the non-linear knee map */
static void street_speedo(uint8_t alpha)
{
    char buf[16];
    const GaugeSpec g = { (float)DASH_LX(193), (float)DASH_LY(186), (float)DASH_LR(156),
                          DASH_LR(16) };
    const bool ok = dash_ch_valid(&g_dash, DASH_CH_SPEED);
    const float mph = g_dash.ch.speed_mph;

    draw_gauge_chrome(&g, alpha);

    float tick_fracs[11];
    for (uint8_t i = 0U; i < 11U; i++)
    {
        tick_fracs[i] = dash_speed_frac((float)(i * 20U));
    }
    draw_gauge_ticks(&g, tick_fracs, 11U, alpha);

    /* tick labels at r66/88; 160/180/200 smaller + dimmer past the knee */
    for (uint8_t i = 0U; i < 11U; i++)
    {
        const uint16_t v = (uint16_t)(i * 20U);
        float lx;
        float ly;
        dash_arc_point(g.cx, g.cy, g.r * DASH_GAUGE_LABEL_R_FRAC, tick_fracs[i], &lx, &ly);
        const bool knee = (v > DASH_SPEED_KNEE);
        dash_color(knee ? COLOR_TICK_DIM : COLOR_LABEL, knee ? DA(200) : alpha);
        snprintf(buf, sizeof(buf), "%u", (unsigned)v);
        EVE_cmd_text((int16_t)lx, (int16_t)(ly - (float)DASH_LR(7)),
                     dash_font(DF_TINY), EVE_OPT_CENTER, buf);
    }

    if (ok && (mph > 0.5f))
    {
        dash_color(COLOR_ACCENT, alpha);
        draw_arc(g.cx, g.cy, g.r, 0.0f, dash_speed_frac(mph), g.stroke);
    }

    draw_gauge_needle_hub(&g, ok ? dash_speed_frac(mph) : 0.0f, ok, alpha);

    /* hub readout below center */
    dash_color(COLOR_VALUE, alpha);
    if (ok)
    {
        snprintf(buf, sizeof(buf), "%d", (int)(mph + 0.5f));
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text((int16_t)g.cx, DASH_LY(255), dash_font(DF_BIG), EVE_OPT_CENTER, buf);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text((int16_t)g.cx, DASH_LY(292), dash_font(DF_LABEL), EVE_OPT_CENTERX, "MPH");
}

static void street_tach(uint8_t alpha)
{
    char buf[16];
    const GaugeSpec g = { (float)DASH_LX(487), (float)DASH_LY(182), (float)DASH_LR(101),
                          DASH_LR(10) };
    const bool ok = dash_ch_valid(&g_dash, DASH_CH_RPM);
    const float rpm = g_dash.ch.rpm;
    const float frac = ok ? (rpm / (float)DASH_REDLINE_RPM) : 0.0f;

    draw_gauge_chrome(&g, alpha);

    /* static red zone 7600..8000 */
    dash_color(COLOR_REDZONE, alpha);
    draw_arc(g.cx, g.cy, g.r,
             (float)DASH_SHIFT_RPM / (float)DASH_REDLINE_RPM, 1.0f, g.stroke);

    float tick_fracs[9];
    for (uint8_t i = 0U; i < 9U; i++)
    {
        tick_fracs[i] = (float)i / 8.0f;
    }
    draw_gauge_ticks(&g, tick_fracs, 9U, alpha);
    for (uint8_t i = 0U; i < 9U; i++)
    {
        float lx;
        float ly;
        dash_arc_point(g.cx, g.cy, g.r * DASH_GAUGE_LABEL_R_FRAC, tick_fracs[i], &lx, &ly);
        dash_color((8U == i) ? COLOR_MUTED_RED : COLOR_LABEL, alpha);
        snprintf(buf, sizeof(buf), "%u", (unsigned)i);
        EVE_cmd_text((int16_t)lx, (int16_t)(ly - (float)DASH_LR(7)),
                     dash_font(DF_TINY), EVE_OPT_CENTER, buf);
    }

    if (ok && (frac > 0.005f))
    {
        const DashColorState rc = dash_rpm_color(rpm);
        dash_color((DASH_COLOR_RED == rc) ? COLOR_RED_FILL : COLOR_ACCENT, alpha);
        draw_arc(g.cx, g.cy, g.r, 0.0f, (frac > 1.0f) ? 1.0f : frac, g.stroke);
    }

    draw_gauge_needle_hub(&g, (frac > 1.0f) ? 1.0f : frac, ok, alpha);

    const DashColorState rc = ok ? dash_rpm_color(rpm) : DASH_COLOR_NORMAL;
    dash_color(dash_state_text_color(rc), alpha);
    if (ok)
    {
        snprintf(buf, sizeof(buf), "%d", (int)(rpm + 0.5f));
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text((int16_t)g.cx, DASH_LY(228), dash_font(DF_MID), EVE_OPT_CENTER, buf);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text((int16_t)g.cx, DASH_LY(252), dash_font(DF_TINY), EVE_OPT_CENTERX, "RPM");
}

/* telltale row: dead-front dots; lit = filled + glow + colored label; a
 * channel with NO data renders the distinct muted state (KTD4/R8 note) */
static void street_telltales(uint8_t alpha)
{
    const bool oilp_ok = dash_ch_valid(&g_dash, DASH_CH_OILP);
    const bool oilt_ok = dash_ch_valid(&g_dash, DASH_CH_OILT);
    const struct Telltale tt[4] = {
        { "FUEL", dash_ch_valid(&g_dash, DASH_CH_FUEL),
          dash_ch_valid(&g_dash, DASH_CH_FUEL) &&
              (DASH_COLOR_AMBER == dash_fuel_state(g_dash.ch.fuel_gal)),
          COLOR_AMBER },
        { "OIL", oilp_ok || oilt_ok,
          dash_telltale_oil(g_dash.ch.oil_press_psi, oilp_ok, g_dash.ch.oil_temp_f, oilt_ok),
          COLOR_RED_FILL },
        { "ECT", dash_ch_valid(&g_dash, DASH_CH_ECT),
          dash_ch_valid(&g_dash, DASH_CH_ECT) &&
              (DASH_COLOR_RED == dash_ect_state(g_dash.ch.ect_f)),
          COLOR_RED_FILL },
        { "VOLTS", dash_ch_valid(&g_dash, DASH_CH_VOLTS),
          dash_ch_valid(&g_dash, DASH_CH_VOLTS) &&
              (DASH_COLOR_RED == dash_volts_state(g_dash.ch.volts)),
          COLOR_RED_FILL },
    };

    /* four groups (dot + label) centered with 26 px mock gaps */
    const int16_t cy = DASH_LY(346);
    const int16_t dot_r = (int16_t)(DASH_LR(9) / 2);
    int16_t x = DASH_LX(214); /* row roughly centered for 4 groups */
    for (uint8_t i = 0U; i < 4U; i++)
    {
        uint32_t dot_col = COLOR_DEAD_DOT;
        uint32_t lab_col = COLOR_HUB_RING;
        uint8_t a = DA(230);
        if (!tt[i].valid)
        {
            dot_col = COLOR_NODATA; /* lost sensor is never "confirmed OK" */
            lab_col = COLOR_NODATA;
        }
        else if (tt[i].lit)
        {
            dot_col = tt[i].color;
            lab_col = tt[i].color;
            a = alpha;
            dash_color(dot_col, DA(60)); /* glow */
            EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)((dot_r + DASH_LR(5)) * 16));
            EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
            EVE_cmd_dl(VERTEX2F((int16_t)(x * 16), (int16_t)(cy * 16)));
            EVE_cmd_dl(DL_END);
        }
        dash_color(dot_col, a);
        EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)(dot_r * 16));
        EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
        EVE_cmd_dl(VERTEX2F((int16_t)(x * 16), (int16_t)(cy * 16)));
        EVE_cmd_dl(DL_END);
        dash_color(lab_col, a);
        EVE_cmd_text((int16_t)(x + DASH_LX(10)), (int16_t)(cy - DASH_LR(7)),
                     dash_font(DF_TINY), 0U, tt[i].label);
        x = (int16_t)(x + DASH_LX(48));
    }
}

void draw_street_mode(uint32_t now_ms, uint8_t alpha)
{
    (void)now_ms;
    char buf[24];

    street_speedo(alpha);
    street_tach(alpha);
    street_telltales(alpha);

    /* odometer, bottom center */
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH_LX(258), DASH_LY(368), dash_font(DF_TINY), 0U, "ODOMETER");
    dash_color(COLOR_ODO, alpha);
    snprintf(buf, sizeof(buf), "%.1f", (double)dash_odo_miles(&g_odo));
    EVE_cmd_text(DASH_LX(310), DASH_LY(378), dash_font(DF_SMALL), EVE_OPT_CENTER, buf);
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH_LX(352), DASH_LY(368), dash_font(DF_TINY), 0U, "MI");
}

/* ---- alarm takeover (U9): preempts both modes at ~2.8 Hz ---- */
void draw_alarm_takeover(DashAlarm alarm, uint32_t now_ms, uint8_t alpha)
{
    char buf[24];
    const bool bright = dash_flash_phase(now_ms, DASH_ALARM_FLASH_HALF_MS);

    if (bright)
    {
        /* vertical gradient approximation of the spec's red radial */
        EVE_cmd_dl(COLOR_A(alpha));
        EVE_cmd_gradient(0, 0, 0xD92020UL, 0, (int16_t)EVE_VSIZE, 0x700C0CUL);
    }
    else
    {
        dash_color(0x260606UL, alpha);
        EVE_cmd_dl(DL_LINE_WIDTH | 16UL);
        EVE_cmd_dl(DL_BEGIN | EVE_RECTS);
        EVE_cmd_dl(VERTEX2F(0, 0));
        /* bottom-right corner: EVE_HSIZE*16 = 16384 overflows VERTEX2F's
         * signed 15-bit field (the splash-round wraparound lesson), so the
         * rect ends at the last representable 1/16 px inside the panel */
        EVE_cmd_dl(VERTEX2F((int16_t)(EVE_HSIZE * 16 - 1), (int16_t)(EVE_VSIZE * 16)));
        EVE_cmd_dl(DL_END);
    }

    const int16_t cx = (int16_t)(EVE_HSIZE / 2U);
    const char *title = (DASH_ALARM_OILP == alarm) ? "OIL PRESSURE" :
                        (DASH_ALARM_OILT == alarm) ? "OIL TEMP" : "COOLANT TEMP";
    /* limit text derives from the same constants dash_alarm_classify()
     * compares against, so a threshold tune can never leave the banner
     * showing a stale number */
    char limit[24];
    if (DASH_ALARM_OILP == alarm)
    {
        snprintf(limit, sizeof(limit), "MINIMUM %d PSI", (int)DASH_OILP_RED_PSI);
    }
    else
    {
        snprintf(limit, sizeof(limit), "MAXIMUM %d F",
                 (DASH_ALARM_OILT == alarm) ? (int)DASH_OILT_RED_F : (int)DASH_ECT_RED_F);
    }
    const float live = (DASH_ALARM_OILP == alarm) ? g_dash.ch.oil_press_psi :
                       (DASH_ALARM_OILT == alarm) ? g_dash.ch.oil_temp_f : g_dash.ch.ect_f;

    dash_color(COLOR_ALARM_TXT, alpha);
    EVE_cmd_text(cx, DASH_LY(120), dash_font(DF_LABEL), EVE_OPT_CENTER, "WARNING");
    dash_color(COLOR_VALUE, alpha);
    EVE_cmd_text(cx, DASH_LY(165), dash_font(DF_TITLE), EVE_OPT_CENTER, title);
    snprintf(buf, sizeof(buf), "%d", (int)(live + 0.5f));
    EVE_cmd_text(cx, DASH_LY(245), dash_font(DF_HERO), EVE_OPT_CENTER, buf);
    dash_color(COLOR_ALARM_TXT, alpha);
    EVE_cmd_text(cx, DASH_LY(330), dash_font(DF_LABEL), EVE_OPT_CENTER, limit);
}

/* One dash composition: fonts registered, then alarm-or-mode content. The
 * alarm classifier reads valid channels only, so a missing sensor can never
 * assert (or hold) a takeover (R10/R11). */
void draw_dash_content(uint32_t now_ms, uint8_t alpha)
{
    dash_register_fonts();

    const DashAlarm alarm = dash_alarm_classify(&g_dash);
    if (DASH_ALARM_NONE != alarm)
    {
        draw_alarm_takeover(alarm, now_ms, alpha);
    }
    else if (DASH_MODE_STREET == g_dash.mode)
    {
        draw_street_mode(now_ms, alpha);
    }
    else
    {
        draw_track_mode(now_ms, alpha);
    }
    EVE_cmd_dl(COLOR_A(255U));
}

/* Build one un-swapped frame for a mode and read back its display-list
 * usage (REG_CMD_DL, bytes -> words). Boot-time diagnostic only (KTD8);
 * the DL is discarded by the next CMD_DLSTART. */
uint16_t measure_mode_dl(DashMode mode)
{
    const DashMode saved = g_dash.mode;
    g_dash.mode = mode;
    EVE_cmd_dl(CMD_DLSTART);
    EVE_cmd_dl(DL_CLEAR_COLOR_RGB | COLOR_BG);
    EVE_cmd_dl(DL_CLEAR | CLR_COL | CLR_STN | CLR_TAG);
    draw_dash_content(0UL, 255U);
    EVE_execute_cmd();
    const uint32_t dl_bytes = EVE_memRead32(REG_CMD_DL);
    g_dash.mode = saved;
    return (uint16_t)(dl_bytes / 4UL);
}

/* The standing display: one full dash frame, swapped in. */
void dash_frame(uint32_t now_ms)
{
    eve_frame_begin(COLOR_BG);
    draw_dash_content(now_ms, 255U);
    eve_frame_end();
}

/* Play the 2000 ms splash, then crossfade ~400 ms into the pony screen.
 * Blocking; called once from setup() after the assets are in RAM_G. */
void run_splash(const ThemeDesc *theme)
{
    uint32_t frames = 0UL;
    const uint32_t t0 = millis();

    /* animation: rebuild the display list every frame */
    for (;;)
    {
        const uint32_t now = millis() - t0;
        const uint32_t t = min(now, SPLASH_DURATION_MS);

        eve_frame_begin(0x000000UL);
        draw_splash_elements(theme, t, 255U);
        eve_frame_end();

        if (0UL == frames)
        {
            /* first frame is on screen -- now it is safe to light the panel */
            set_backlight(BL_STEADY);
        }
        frames++;

        if (splash_done(now)) { break; }
    }

    Serial.printf("Splash: %lu frames in %lu ms (target >= 100 in 2000)\r\n",
                  (unsigned long)frames, (unsigned long)SPLASH_DURATION_MS);

    /* crossfade: splash final frame out, live dash in (R17 -- direct
     * crossfade; splash draws from flash, dash text from RAM_G fonts, so
     * both are resident with no memory contention) */
    const uint32_t f0 = millis();
    for (;;)
    {
        const uint32_t fnow = millis() - f0;
        const uint32_t ft = min(fnow, CROSSFADE_MS);
        const uint8_t in_a = (uint8_t)((ft * 255UL) / CROSSFADE_MS);
        const uint8_t out_a = (uint8_t)(255U - in_a);

        eve_frame_begin(COLOR_BG);
        draw_splash_elements(theme, SPLASH_DURATION_MS, out_a);
        draw_dash_content(millis(), in_a);
        eve_frame_end();

        if (ft >= CROSSFADE_MS) { break; }
    }
}
