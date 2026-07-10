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
 *      (CMD_FLASHUPDATE, sector 0 never touched); decodes the pony PNG into
 *      RAM_G -- all with the backlight held dark so nothing flashes
 *   4. plays the 2000 ms animated splash (assets/splash/README.md spec,
 *      timing in splash_timeline.h; theme picked in splash_config.h),
 *      rendering ASTC bitmaps directly from flash, lighting the backlight
 *      only after the first frame is on screen
 *   5. crossfades ~400 ms into the pony screen: dark background, galloping
 *      pony logo, red/white/blue tri-bar, and "MUSTANG" in ROM font 31 --
 *      the standing display (steady 100% backlight at 10 kHz PWM)
 *
 * Serial is 115200 8N1. After the splash it prints the rendered frame
 * count -- >= 100 frames in the 2000 ms window means the >= 50 fps target.
 *
 * Note: this project also builds offline with a minimal arduino-cli Teensy
 * platform that does not run the arduino .ino prototype generator, so every
 * function below is declared before it is used.
 */

#include <Arduino.h>
#include <SPI.h>
#include "EVE.h"
#include "pony_png.h"
#include "splash_config.h"
#include "splash_timeline.h"
#include "splash_flash.h" /* after EVE.h so its guarded EVE_ASTC_* defer to the library */

/* Teensy 4 USB device state: non-zero once the USB host has configured us
 * (cores/teensy4/usb.c). Zero when running from a wall adapter / car power. */
extern "C" {
extern volatile uint8_t usb_configuration;
}

/* ---- asset plumbing ---- */

/* Splash assets are ASTC bitmaps resident in the panel's 64 MB QSPI flash
 * (address table + provisioning pack in splash_flash.h) and are rendered
 * directly from flash. RAM_G is now UNUSED by the splash -- only the pony
 * screen's PNG decode below occupies it (dash fonts arrive in a later
 * unit). */

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

/* The pony PNG still decodes into RAM_G (address 0); a later unit retires
 * the pony screen. */
struct LoadedAsset
{
    uint32_t addr;
    uint16_t fmt;
    uint16_t w;
    uint16_t h;
};

enum
{
    L_PONY = 0,
    L_COUNT
};

static LoadedAsset g_loaded[L_COUNT];

/* RAM_G scratch used only while provisioning the splash pack at boot:
 * a 64-byte header readback at the top of RAM_G and a 32 KB staging
 * buffer for CMD_FLASHUPDATE chunks just below it -- both far above the
 * pony bitmap at address 0 (~288 KB). */
static const uint32_t SCRATCH_HDR = 0xFF000UL;
static const uint32_t SCRATCH_BUF = 0xF7000UL;
static const uint32_t PROVISION_CHUNK = 0x8000UL; /* 32 KB, multiple of 4096 */

/* ---- colours (0xRRGGBB) ---- */
static const uint32_t COLOR_BG       = 0x0A0E14UL; /* near-black, faint blue */
static const uint32_t COLOR_PONY     = 0xE8E8ECUL; /* silver-white */
static const uint32_t COLOR_TITLE    = 0xFFFFFFUL; /* white */
static const uint32_t COLOR_BAR_R    = 0xC8102EUL; /* tri-bar red */
static const uint32_t COLOR_BAR_W    = 0xF5F5F5UL; /* tri-bar white */
static const uint32_t COLOR_BAR_B    = 0x1B3E8CUL; /* tri-bar blue */

/* steady backlight duty: 128 = 100% (the EVE PWM scale tops out at 128) */
static const uint8_t BL_STEADY = 128U;

static const uint32_t CROSSFADE_MS = 400UL;

static bool eve_ready = false; /* set true only if EVE_init() reported E_OK */

/* ---- forward declarations (explicit prototypes, see note above) ---- */
void set_backlight(uint8_t duty);
void eve_frame_begin(uint32_t clear_rgb);
void eve_frame_end(void);
void load_pony_asset(void);
bool splash_header_current(void);
bool splash_flash_provision(void);
void draw_flash_asset(const SplashFlashAsset *a, int16_t x, int16_t y);
void draw_splash_background(const SplashFlashAsset *bg, uint8_t alpha);
void draw_splash_emblem(const SplashFlashAsset *emblem, float scale, uint8_t alpha);
void draw_splash_elements(const ThemeDesc *theme, uint32_t now_ms, uint8_t global_alpha);
void draw_pony_elements(uint8_t alpha);
void draw_dash(void);
void run_splash(const ThemeDesc *theme);

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

    if (E_OK == ret)
    {
        eve_ready = true;

        /* Backlight stays dark through flash provisioning + the pony PNG
         * decode so power-up never flashes a lit blank panel; run_splash()
         * lights it once the first splash frame is on screen. */
        set_backlight(0U);

        load_pony_asset();

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

        if (splash_ok)
        {
            const ThemeDesc *theme = &THEMES[g_theme];
            run_splash(theme); /* 2000 ms animation + 400 ms crossfade, blocking */
        }

        draw_dash(); /* the standing pony screen */
        set_backlight(BL_STEADY); /* no-op after the splash; lights the panel if it was skipped */
        Serial.println(F("EVE init OK: splash played, pony screen up, backlight steady at 100%."));
    }
    else
    {
        Serial.println(F("EVE_init() did NOT return E_OK - see failure guide in the summary."));
        Serial.println(F("Halting render; will still print this once. Check wiring / power / SPI."));
    }
}

void loop(void)
{
    /* static image, steady backlight: nothing to do */
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

/* Close the display list, swap it in, and wait for the co-processor. */
void eve_frame_end(void)
{
    EVE_cmd_dl(DL_DISPLAY);
    EVE_cmd_dl(CMD_SWAP);
    EVE_execute_cmd();
}

/* Decode the pony PNG into RAM_G at address 0 (~288 KB as ARGB4). The
 * splash renders straight from QSPI flash, so this is RAM_G's only tenant. */
void load_pony_asset(void)
{
    g_loaded[L_PONY].addr = 0UL;
    g_loaded[L_PONY].fmt = EVE_ARGB4;
    g_loaded[L_PONY].w = PONY_PNG_WIDTH;
    g_loaded[L_PONY].h = PONY_PNG_HEIGHT;
    EVE_cmd_loadimage(0UL, EVE_OPT_NODL, pony_png, sizeof(pony_png));
    EVE_execute_cmd(); /* wait for the on-chip decode to finish */
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
    for (uint32_t off = 0UL; off < SPLASH_FLASH_PACK_SIZE; off += PROVISION_CHUNK)
    {
        const uint32_t n = min(PROVISION_CHUNK, SPLASH_FLASH_PACK_SIZE - off);
        EVE_memWrite_flash_buffer(SCRATCH_BUF, &splash_flash_pack[off], n);
        EVE_cmd_flashupdate(SPLASH_FLASH_BASE + off, SCRATCH_BUF, n);
        Serial.print('.');
    }
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

/* The pony screen's elements (logo + tri-bar + wordmark) at a given alpha,
 * shared by the standing screen (alpha 255) and the crossfade ramp. */
void draw_pony_elements(uint8_t alpha)
{
    /* layout (1024 x 600): logo centered up top, tri-bar accent below it,
     * MUSTANG wordmark at the bottom */
    const int16_t logo_x = (int16_t)((EVE_HSIZE - PONY_PNG_WIDTH) / 2U); /* 272 */
    const int16_t logo_y = 90;
    const int16_t bar_w = 18, bar_h = 56, bar_gap = 8;
    const int16_t bars_x = (int16_t)((EVE_HSIZE - (3U * bar_w + 2U * bar_gap)) / 2U);
    const int16_t bars_y = 406;
    const LoadedAsset *pony = &g_loaded[L_PONY];

    EVE_cmd_dl(COLOR_A(alpha));

    /* pony logo bitmap (decoded PNG in RAM_G), tinted silver-white */
    EVE_color_rgb(COLOR_PONY);
    EVE_cmd_setbitmap(pony->addr, pony->fmt, pony->w, pony->h);
    EVE_cmd_dl(DL_BEGIN | EVE_BITMAPS);
    EVE_cmd_dl(VERTEX2F((int16_t)(logo_x * 16), (int16_t)(logo_y * 16)));
    EVE_cmd_dl(DL_END);

    /* tri-bar accent: red / white / blue */
    EVE_cmd_dl(DL_LINE_WIDTH | 16UL); /* 1 px corner radius */
    EVE_cmd_dl(DL_BEGIN | EVE_RECTS);
    EVE_color_rgb(COLOR_BAR_R);
    EVE_cmd_dl(VERTEX2F((int16_t)(bars_x * 16), (int16_t)(bars_y * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)((bars_x + bar_w) * 16), (int16_t)((bars_y + bar_h) * 16)));
    EVE_color_rgb(COLOR_BAR_W);
    EVE_cmd_dl(VERTEX2F((int16_t)((bars_x + bar_w + bar_gap) * 16), (int16_t)(bars_y * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)((bars_x + 2 * bar_w + bar_gap) * 16), (int16_t)((bars_y + bar_h) * 16)));
    EVE_color_rgb(COLOR_BAR_B);
    EVE_cmd_dl(VERTEX2F((int16_t)((bars_x + 2 * (bar_w + bar_gap)) * 16), (int16_t)(bars_y * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)((bars_x + 3 * bar_w + 2 * bar_gap) * 16), (int16_t)((bars_y + bar_h) * 16)));
    EVE_cmd_dl(DL_END);

    /* wordmark */
    EVE_color_rgb(COLOR_TITLE);
    EVE_cmd_text((int16_t)(EVE_HSIZE / 2U), 512, 31U, EVE_OPT_CENTER, "MUSTANG");

    EVE_cmd_dl(COLOR_A(255U));
}

/* Build and swap in the standing pony screen. */
void draw_dash(void)
{
    eve_frame_begin(COLOR_BG);
    draw_pony_elements(255U);
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

    /* crossfade: splash final frame out, pony screen in */
    const uint32_t f0 = millis();
    for (;;)
    {
        const uint32_t fnow = millis() - f0;
        const uint32_t ft = min(fnow, CROSSFADE_MS);
        const uint8_t in_a = (uint8_t)((ft * 255UL) / CROSSFADE_MS);
        const uint8_t out_a = (uint8_t)(255U - in_a);

        eve_frame_begin(COLOR_BG);
        draw_splash_elements(theme, SPLASH_DURATION_MS, out_a);
        draw_pony_elements(in_a);
        eve_frame_end();

        if (ft >= CROSSFADE_MS) { break; }
    }
}
