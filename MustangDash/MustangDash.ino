/*
 * MustangDash - Riverdi triple-panel EVE4 dash on a Teensy 4.1
 *
 * Displays: center SM-RVT70HSBNWN00 (7.0" 1024x600) + left/right
 *           SM-RVT50HQBNWN00 (5.0" 800x480), all BT817 / EVE4, no touch
 * MCU     : Teensy 4.1 (IMXRT1062)
 * Bus     : hardware SPI0 shared by all three (SCLK=13, MISO=12, MOSI=11)
 * Control : per-panel CS/PD from dash_panels.h -- center 14/17, left 15/20,
 *           right 16/21 (INT not connected -> polling only)
 *
 * Library : RudolphRiedel/FT800-FT813 (EmbeddedVideoEngine, v5.x) with the
 *           vendored multi-panel patch (EVE_select_panel); center profile
 *           EVE_RVT70H compiled, side timings carried at runtime
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
 * Rendering lives in sibling single-TU headers: dash_draw.h (shared
 * primitives), dash_render.h (center TRACK/STREET/alarm), engine_render.h
 * (left 5" ENGINE screen), timing_render.h (right 5" TIMING/ROAD screen),
 * and splash_render.h (flash provisioning + splash + crossfade).
 * This file keeps setup/loop and the glue: panel selection, EVE frame
 * plumbing, cluster brightness, fonts, serial pump, odometer EEPROM, and
 * the shared state every header reads.
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
#include "dash_panels.h" /* per-panel pins + timings (host-tested); mapped to EVE_panel_t in setup() */

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
    uint32_t metrics_addr; /* 148-byte block, passed to CMD_SETFONT2 (same layout on every panel's RAM_G) */
    uint8_t ok_mask;       /* bit per panel; a clear bit -> ROM font 31 fallback on that panel only */
};

static DashFontLoaded g_fonts[DASH_FONT_COUNT];
static DashState g_dash;
static DashSimState g_sim;
static DashOdo g_odo;

/* ---- panel plumbing (three BT817s on one shared SPI bus, KTD1/KTD9) ---- */
static EVE_panel_t g_eve_panels[DASH_PANEL_COUNT]; /* library form of DASH_PANELS, filled in setup() */
static bool g_panel_ok[DASH_PANEL_COUNT];          /* init succeeded; a dead panel stays dark, never blocks the others (R9) */
static uint8_t g_active_panel = DASH_PANEL_CENTER; /* which panel the EVE library is currently routed at */
static uint8_t g_dash_brightness = 0U;             /* ONE cluster brightness (R12); set to BL_STEADY at boot_complete */

static char g_serial_line[DASH_SERIAL_MAX_LINE + 17]; /* headroom to detect too-long */
static uint8_t g_serial_len = 0U;

static uint32_t g_loop_last_ms = 0UL;
static uint16_t g_fps = 0U;         /* frames completed in the last full second */
static uint16_t g_fps_frames = 0U;
static uint32_t g_fps_window_ms = 0UL;
static uint16_t g_dl[DASH_PANEL_COUNT][2];          /* boot-measured DL words per panel x {track, street} */
static uint32_t g_eve_faults[DASH_PANEL_COUNT];     /* coprocessor faults auto-recovered by EVE_busy(), per panel */

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

/* Post-init SPI operating point (R11/KTD8). All three EVE_init()s run at the
 * conservative 8 MHz; the bus then rises to this once. 24 MHz is the first
 * candidate (panels rated 30 MHz post-config); U9's bench read-integrity
 * soak validates or tunes it -- fps alone never accepts an operating point. */
static const uint32_t DASH_SPI_RUN_HZ = 24000000UL;

static bool eve_ready = false; /* center panel initialized OK (sides have their own g_panel_ok flags) */

/* ---- forward declarations (explicit prototypes, see note above) ---- */
void set_backlight(uint8_t duty);
void eve_frame_begin(uint32_t clear_rgb);
void eve_frame_end(void);
bool dash_select_panel(uint8_t idx);
void dash_set_brightness(uint8_t duty);
void dash_sides_frame(uint8_t alpha);
bool load_dash_fonts(uint8_t panel);
uint16_t dash_font(uint8_t idx);
void dash_register_fonts(void);
uint16_t measure_side_dl(uint8_t panel, DashMode mode);
void odo_eeprom_load(void);
void odo_eeprom_write(void);
void pump_serial(void);
void handle_serial_line(const char *line);

/* Renderers (single-TU headers): shared primitives first, then the center,
 * the two sides, and the splash last -- run_splash() calls draw_dash_content()
 * and dash_sides_frame() during the crossfade. All read the state above. */
#include "dash_draw.h"
#include "dash_render.h"
#include "engine_render.h"
#include "timing_render.h"
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
    Serial.println(F("=== MustangDash / Riverdi triple dash (BT817 x3) on Teensy 4.1 ==="));
    static const char *const kPanelNames[DASH_PANEL_COUNT] = { "CENTER", "LEFT", "RIGHT" };
    for (uint8_t p = 0U; p < DASH_PANEL_COUNT; p++)
    {
        Serial.printf("Panel %s: CS=%u PD=%u %ux%u\r\n", kPanelNames[p],
                      (unsigned)DASH_PANELS[p].cs_pin, (unsigned)DASH_PANELS[p].pd_pin,
                      (unsigned)DASH_PANELS[p].width, (unsigned)DASH_PANELS[p].height);
    }
    Serial.printf("Splash theme: %u (0=blue 1=red 2=checkered)\r\n", (unsigned)g_theme);

    /* Map the host-tested descriptor table into the library's panel form and
     * drive every panel's control pins from the MCU (CS idle high, PD held
     * low until that panel's EVE_init() sequences it). */
    for (uint8_t p = 0U; p < DASH_PANEL_COUNT; p++)
    {
        const DashPanelDesc *d = &DASH_PANELS[p];
        EVE_panel_t *e = &g_eve_panels[p];
        e->cs_pin = d->cs_pin;
        e->pdn_pin = d->pd_pin;
        e->slot = p;
        e->pclk = d->pclk_div;
        e->pclk_freq = d->pclk_freq;
        e->hsize = d->width;
        e->vsize = d->height;
        e->hcycle = d->hcycle;
        e->hoffset = d->hoffset;
        e->hsync0 = d->hsync0;
        e->hsync1 = d->hsync1;
        e->vcycle = d->vcycle;
        e->voffset = d->voffset;
        e->vsync0 = d->vsync0;
        e->vsync1 = d->vsync1;
        e->swizzle = d->swizzle;
        e->pclkpol = d->pclkpol;
        e->cspread = d->cspread;
        pinMode(d->cs_pin, OUTPUT);
        digitalWrite(d->cs_pin, HIGH);
        pinMode(d->pd_pin, OUTPUT);
        digitalWrite(d->pd_pin, LOW);
    }

    /* SPI mode 0, MSB first; the clock stays conservative through every
     * panel's init (BT817 needs <= 11 MHz until configured), then rises
     * once, bus-wide, to the R11 operating point. */
    SPI.begin();
    SPI.beginTransaction(SPISettings(8UL * 1000000UL, MSBFIRST, SPI_MODE0));
    Serial.println(F("SPI up at 8 MHz, mode 0 (init)."));

    /* Per-panel init (KTD9): select -> EVE_init with that panel's timings ->
     * REG_ID check -> backlight forced dark immediately (the library's init
     * leaves 25% duty; the dark-boot contract holds until boot_complete).
     * A dead panel eats the library's bounded REG_ID timeout and is then
     * simply skipped everywhere (R9). */
    for (uint8_t p = 0U; p < DASH_PANEL_COUNT; p++)
    {
        (void)EVE_select_panel(&g_eve_panels[p]); /* pre-init: routes pins + timings only */
        g_active_panel = p;
        const uint8_t ret = EVE_init();
        const uint8_t reg_id = EVE_memRead8(REG_ID);
        set_backlight(0U);
        g_panel_ok[p] = (E_OK == ret);
        Serial.printf("Panel %s: EVE_init 0x%02X (E_OK=0x00), REG_ID 0x%02X (want 0x7C)\r\n",
                      kPanelNames[p], ret, reg_id);
    }
    eve_ready = g_panel_ok[DASH_PANEL_CENTER];
    const bool any_panel_ok = g_panel_ok[0] || g_panel_ok[1] || g_panel_ok[2];

    /* One bus-wide raise after every init is done (KTD8). */
    SPI.endTransaction();
    SPI.beginTransaction(SPISettings(DASH_SPI_RUN_HZ, MSBFIRST, SPI_MODE0));
    Serial.printf("SPI raised to %lu MHz (U9 read-integrity soak gates this operating point)\r\n",
                  (unsigned long)(DASH_SPI_RUN_HZ / 1000000UL));

    /* Probe the CENTER panel's onboard QSPI flash: attach and switch to fast
     * mode. Center-only -- the sides carry no flash assets this round. A
     * failure is logged, never fatal: the dash must not depend on flash. */
    uint8_t flash_ret = E_NOT_OK;
    if (dash_select_panel(DASH_PANEL_CENTER))
    {
        flash_ret = EVE_init_flash();
        const uint32_t flash_mb = EVE_memRead32(REG_FLASH_SIZE);
        Serial.printf("EVE_init_flash() returned 0x%02X (E_OK=0x00), REG_FLASH_SIZE = %lu MB\r\n",
                      flash_ret, (unsigned long)flash_mb);
    }

    /* Odometer loads regardless of panel state -- it is Teensy-local. */
    dash_state_init(&g_dash);
    dash_sim_init(&g_sim);
    dash_odo_init(&g_odo);
    odo_eeprom_load();
    Serial.printf("Odometer: %.1f mi (trip %.1f)\r\n",
                  (double)dash_odo_miles(&g_odo), (double)dash_trip_miles(&g_odo));

    if (any_panel_ok)
    {
        /* Splash needs its ASTC assets in the CENTER panel's QSPI flash;
         * verify (and provision on first boot / asset change). Skipped --
         * along with the splash itself -- if the center or its flash probe
         * failed: the dash must never depend on flash. */
        bool splash_ok = false;
        if (eve_ready && (E_OK == flash_ret) && dash_select_panel(DASH_PANEL_CENTER))
        {
            splash_ok = splash_flash_provision();
        }
        else
        {
            Serial.println(F("QSPI flash unavailable -> skipping splash (assets live in flash)."));
        }

        /* Dash fonts into every healthy panel's own RAM_G before the splash
         * starts (KTD3/KTD6): each BT817 is independent silicon, so the
         * upload runs once per panel. */
        for (uint8_t p = 0U; p < DASH_PANEL_COUNT; p++)
        {
            if (dash_select_panel(p))
            {
                (void)load_dash_fonts(p);
            }
        }

        /* Seed the first visible dash frame: everything starts invalid,
         * then one sim step fills plausible values -- an uninitialized
         * channel can never flash a false alarm at power-up (KTD3). */
        g_loop_last_ms = millis();
        dash_sim_step(&g_sim, &g_dash, 50U);

        /* One-time DL-usage diagnostic per panel and mode (KTD8): built
         * un-swapped while the panels still show nothing, printed into the
         * banner -- after boot the only serial output is command acks. */
        if (dash_select_panel(DASH_PANEL_CENTER))
        {
            g_dl[DASH_PANEL_CENTER][0] = measure_mode_dl(DASH_MODE_TRACK);
            g_dl[DASH_PANEL_CENTER][1] = measure_mode_dl(DASH_MODE_STREET);
        }
        g_dl[DASH_PANEL_LEFT][0] = measure_side_dl(DASH_PANEL_LEFT, DASH_MODE_TRACK);
        g_dl[DASH_PANEL_LEFT][1] = measure_side_dl(DASH_PANEL_LEFT, DASH_MODE_STREET);
        g_dl[DASH_PANEL_RIGHT][0] = measure_side_dl(DASH_PANEL_RIGHT, DASH_MODE_TRACK);
        g_dl[DASH_PANEL_RIGHT][1] = measure_side_dl(DASH_PANEL_RIGHT, DASH_MODE_STREET);
        Serial.printf("DL usage (track/street of 2048): center %u/%u, left %u/%u, right %u/%u\r\n",
                      (unsigned)g_dl[0][0], (unsigned)g_dl[0][1],
                      (unsigned)g_dl[1][0], (unsigned)g_dl[1][1],
                      (unsigned)g_dl[2][0], (unsigned)g_dl[2][1]);

        Serial.printf("Boot: %lu ms to splash start\r\n", (unsigned long)(millis() - t_start));
        if (splash_ok && dash_select_panel(DASH_PANEL_CENTER))
        {
            const ThemeDesc *theme = &THEMES[g_theme];
            run_splash(theme); /* 2000 ms animation, then the crossfade fades the sides in too (R8) */
        }

        /* boot_complete (KTD9): from here the unified cluster brightness is
         * the only brightness path -- one value, every healthy panel. */
        dash_set_brightness(BL_STEADY);
        Serial.printf("Boot: dash live at %lu ms. Serial is command-only from here (try 'help').\r\n",
                      (unsigned long)(millis() - t_start));
    }
    else
    {
        Serial.println(F("No panel initialized - dash rendering disabled."));
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

    if (!g_panel_ok[DASH_PANEL_CENTER] && !g_panel_ok[DASH_PANEL_LEFT] && !g_panel_ok[DASH_PANEL_RIGHT])
    {
        return;
    }

    /* Sequential per-panel render (KTD8): center first (mode or alarm
     * takeover), then both sides (mode content only -- R4). Dead panels are
     * skipped inside the helpers (R9). */
    if (dash_select_panel(DASH_PANEL_CENTER))
    {
        dash_frame(now);
    }
    dash_sides_frame(255U);

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
            g_eve_faults[g_active_panel]++;
        }
    }
}

/* Route the EVE library at a panel. Refuses for dead or unknown panels so
 * every caller inherits the R9 skip-a-dark-panel behavior for free. */
bool dash_select_panel(uint8_t idx)
{
    if ((idx >= DASH_PANEL_COUNT) || !g_panel_ok[idx])
    {
        return false;
    }
    if (E_OK != EVE_select_panel(&g_eve_panels[idx]))
    {
        return false;
    }
    g_active_panel = idx;
    return true;
}

/* ONE cluster brightness (R12): the only steady-state brightness path.
 * Writes the same duty to every healthy panel in a single call; there is
 * no per-panel brightness anywhere above this line. */
void dash_set_brightness(uint8_t duty)
{
    g_dash_brightness = duty;
    for (uint8_t p = 0U; p < DASH_PANEL_COUNT; p++)
    {
        if (dash_select_panel(p))
        {
            set_backlight(duty);
        }
    }
    (void)dash_select_panel(DASH_PANEL_CENTER);
}

/* Render both side screens for this frame -- mode content only, the alarm
 * takeover is center-only (R4). The first call lights the side backlights
 * so the R8 fade-in is visible (KTD9's boot carve-out); dead panels are
 * skipped (R9). Leaves the center selected for the caller. */
void dash_sides_frame(uint8_t alpha)
{
    static bool sides_lit = false;
    if (!sides_lit)
    {
        for (uint8_t p = DASH_PANEL_LEFT; p <= DASH_PANEL_RIGHT; p++)
        {
            if (dash_select_panel(p))
            {
                set_backlight((0U == g_dash_brightness) ? BL_STEADY : g_dash_brightness);
            }
        }
        sides_lit = true;
    }

    if (dash_select_panel(DASH_PANEL_LEFT))
    {
        eve_frame_begin(COLOR_BG);
        dash_register_fonts();
        if (DASH_MODE_STREET == g_dash.mode)
        {
            engine_street_screen(alpha);
        }
        else
        {
            engine_track_screen(alpha);
        }
        eve_frame_end();
    }
    if (dash_select_panel(DASH_PANEL_RIGHT))
    {
        eve_frame_begin(COLOR_BG);
        dash_register_fonts();
        if (DASH_MODE_STREET == g_dash.mode)
        {
            timing_street_screen(alpha);
        }
        else
        {
            timing_track_screen(alpha);
        }
        eve_frame_end();
    }
    (void)dash_select_panel(DASH_PANEL_CENTER);
}

/* Build one un-swapped side frame and read back its display-list usage
 * (REG_CMD_DL, bytes -> words). Boot diagnostic only, mirrors
 * measure_mode_dl() for the center. */
uint16_t measure_side_dl(uint8_t panel, DashMode mode)
{
    if (!dash_select_panel(panel))
    {
        return 0U;
    }
    const DashMode saved = g_dash.mode;
    g_dash.mode = mode;
    EVE_cmd_dl(CMD_DLSTART);
    EVE_cmd_dl(DL_CLEAR_COLOR_RGB | COLOR_BG);
    EVE_cmd_dl(DL_CLEAR | CLR_COL | CLR_STN | CLR_TAG);
    dash_register_fonts();
    if (DASH_PANEL_LEFT == panel)
    {
        if (DASH_MODE_STREET == mode) { engine_street_screen(255U); } else { engine_track_screen(255U); }
    }
    else
    {
        if (DASH_MODE_STREET == mode) { timing_street_screen(255U); } else { timing_track_screen(255U); }
    }
    EVE_execute_cmd();
    const uint32_t dl_bytes = EVE_memRead32(REG_CMD_DL);
    g_dash.mode = saved;
    (void)dash_select_panel(DASH_PANEL_CENTER);
    return (uint16_t)(dl_bytes / 4UL);
}

/* Inflate every dash font into the SELECTED panel's RAM_G from address 0
 * and patch each metric block's glyph pointer (KTD1/KTD6). The layout is
 * identical on every panel (each BT817 has its own RAM_G), so metrics_addr
 * is shared; success is tracked per panel in ok_mask. One retry per
 * instance; a failed instance falls back to ROM font 31 at render time on
 * that panel only (a degraded screen beats a black one). */
bool load_dash_fonts(uint8_t panel)
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
            g_fonts[i].ok_mask |= (uint8_t)(1U << panel);
        }
        else
        {
            Serial.printf("Font %u inflate FAILED twice on panel %u -> ROM font 31 there\r\n",
                          (unsigned)i, (unsigned)panel);
            g_fonts[i].ok_mask &= (uint8_t)~(1U << panel);
            all_ok = false;
        }

        g_fonts[i].metrics_addr = maddr;
        addr = (gend_expected + 3UL) & ~3UL;
    }

    Serial.printf("RAM_G panel %u: fonts %lu bytes (headroom %lu)\r\n", (unsigned)panel,
                  (unsigned long)addr, (unsigned long)(EVE_RAM_G_SIZE - addr));
    return all_ok;
}

/* Render-time font selector: the instance's bitmap handle, or ROM font 31
 * when that instance failed to load on the currently selected panel. */
uint16_t dash_font(uint8_t idx)
{
    const bool ok = (0U != ((g_fonts[idx].ok_mask >> g_active_panel) & 1U));
    return ok ? (uint16_t)DASH_FONTS[idx].handle : 31U;
}

/* CMD_SETFONT2 emits display-list commands, so every frame that uses the
 * custom fonts re-registers them at its top (~5 words per instance),
 * per panel -- each panel's DL carries its own registrations. */
void dash_register_fonts(void)
{
    for (uint8_t i = 0U; i < DASH_FONT_COUNT; i++)
    {
        if (0U != ((g_fonts[i].ok_mask >> g_active_panel) & 1U))
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
            Serial.printf("ok status mode=%s fps=%u alarm=%s sim=%s faults=%lu,%lu,%lu",
                          (DASH_MODE_TRACK == g_dash.mode) ? "track" : "street",
                          (unsigned)g_fps,
                          (DASH_ALARM_OILP == alarm) ? "oilp" :
                          (DASH_ALARM_OILT == alarm) ? "oilt" :
                          (DASH_ALARM_CLT == alarm) ? "clt" : "none",
                          g_dash.sim_frozen ? "off" : "on",
                          (unsigned long)g_eve_faults[0],
                          (unsigned long)g_eve_faults[1],
                          (unsigned long)g_eve_faults[2]);
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
            Serial.printf(" odo=%.1f trip=%.1f dl=%u/%u,%u/%u,%u/%u eve=%s,%s,%s\r\n",
                          (double)dash_odo_miles(&g_odo), (double)dash_trip_miles(&g_odo),
                          (unsigned)g_dl[0][0], (unsigned)g_dl[0][1],
                          (unsigned)g_dl[1][0], (unsigned)g_dl[1][1],
                          (unsigned)g_dl[2][0], (unsigned)g_dl[2][1],
                          g_panel_ok[0] ? "ok" : "--",
                          g_panel_ok[1] ? "ok" : "--",
                          g_panel_ok[2] ? "ok" : "--");
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
