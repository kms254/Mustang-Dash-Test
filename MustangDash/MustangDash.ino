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
 *   1. brings the EVE chip out of power-down and runs EVE_init()
 *   2. reads REG_ID (a healthy BT817 returns 0x7C) and reports it on Serial
 *   3. sets a steady backlight: REG_PWM_DUTY = 128 (full brightness; the
 *      PWM scale is 0..128) at 10 kHz PWM -- the library's 4 kHz default is
 *      audible as a whine from the backlight driver, confirmed on this bench
 *   4. decodes the embedded pony PNG on-chip (CMD_LOADIMAGE -> RAM_G) and
 *      draws one static display list: dark background, galloping pony logo,
 *      red/white/blue tri-bar accent, and "MUSTANG" in ROM font 31
 *
 * Serial is 115200 8N1.
 *
 * Note: this project is built offline with a minimal arduino-cli Teensy
 * platform that does not run the arduino .ino prototype generator, so every
 * function below is declared before it is used.
 */

#include <Arduino.h>
#include <SPI.h>
#include "EVE.h"
#include "pony_png.h"

/* ---- forward declarations (explicit prototypes, see note above) ---- */
void draw_dash(void);
void set_backlight(uint8_t duty);

/* raw display-list helpers (the library ships these macros commented out) */
#define VERTEX2F(x, y) ((DL_VERTEX2F) | ((((uint32_t) (x)) & 0x7FFFUL) << 15U) | (((uint32_t) (y)) & 0x7FFFUL))

/* colours (0xRRGGBB) */
static const uint32_t COLOR_BG       = 0x0A0E14UL; /* near-black, faint blue */
static const uint32_t COLOR_PONY     = 0xE8E8ECUL; /* silver-white */
static const uint32_t COLOR_TITLE    = 0xFFFFFFUL; /* white */
static const uint32_t COLOR_BAR_R    = 0xC8102EUL; /* tri-bar red */
static const uint32_t COLOR_BAR_W    = 0xF5F5F5UL; /* tri-bar white */
static const uint32_t COLOR_BAR_B    = 0x1B3E8CUL; /* tri-bar blue */

/* steady backlight duty: 128 = 100% (the EVE PWM scale tops out at 128) */
static const uint8_t BL_STEADY = 128U;

/* RAM_G address of the decoded logo bitmap */
static const uint32_t MEM_LOGO = 0UL;

static bool eve_ready = false; /* set true only if EVE_init() reported E_OK */

void setup(void)
{
    Serial.begin(115200);
    uint32_t t_start = millis();
    while (!Serial && ((millis() - t_start) < 2000U))
    {
        /* wait up to 2s for the USB serial monitor, then continue regardless */
    }

    Serial.println();
    Serial.println(F("=== MustangDash / Riverdi RVT70H (BT817) on Teensy 4.1 ==="));
    Serial.printf("Pins: EVE_CS=%d  EVE_PDN=%d  (SCLK=13 MISO=12 MOSI=11)\r\n",
                  (int)EVE_CS, (int)EVE_PDN);
    Serial.printf("Panel: %lu x %lu, EVE_GEN=%d\r\n",
                  (unsigned long)EVE_HSIZE, (unsigned long)EVE_VSIZE, (int)EVE_GEN);

    /* drive the control pins from the MCU */
    pinMode(EVE_CS, OUTPUT);
    digitalWrite(EVE_CS, HIGH); /* CS idle high */
    pinMode(EVE_PDN, OUTPUT);
    digitalWrite(EVE_PDN, LOW); /* hold in power-down until EVE_init() sequences it */

    /* SPI mode 0, MSB first; keep the clock conservative for init (BT817 needs
     * <= 11 MHz until the chip is configured). 8 MHz is safe and stays fast
     * enough for this single small display list, so we leave it here. */
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

    if (E_OK == ret)
    {
        eve_ready = true;
        set_backlight(BL_STEADY);     /* steady full brightness, no pulsing */

        /* decode the embedded PNG into RAM_G (BT817 does this on-chip);
         * PNG with alpha decodes to ARGB4, tintable at draw time */
        EVE_cmd_loadimage(MEM_LOGO, EVE_OPT_NODL, pony_png, sizeof(pony_png));
        EVE_execute_cmd();            /* wait for the decode to finish */

        draw_dash();                  /* one static display list */
        Serial.println(F("EVE init OK: pony logo drawn, backlight steady at 100%."));
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

/* Build and swap in the dash display list: pony logo + tri-bar + wordmark. */
void draw_dash(void)
{
    /* layout (1024 x 600): logo centered up top, tri-bar accent below it,
     * MUSTANG wordmark at the bottom */
    const int16_t logo_x = (int16_t)((EVE_HSIZE - PONY_PNG_WIDTH) / 2U);   /* 272 */
    const int16_t logo_y = 90;
    const int16_t bar_w = 18, bar_h = 56, bar_gap = 8;
    const int16_t bars_x = (int16_t)((EVE_HSIZE - (3U * bar_w + 2U * bar_gap)) / 2U);
    const int16_t bars_y = 406;

    EVE_cmd_dl(CMD_DLSTART);                              /* start a new display list */
    EVE_cmd_dl(DL_CLEAR_COLOR_RGB | COLOR_BG);            /* set background colour */
    EVE_cmd_dl(DL_CLEAR | CLR_COL | CLR_STN | CLR_TAG);   /* clear colour/stencil/tag */

    /* pony logo bitmap (decoded PNG in RAM_G), tinted silver-white */
    EVE_color_rgb(COLOR_PONY);
    EVE_cmd_setbitmap(MEM_LOGO, EVE_ARGB4, PONY_PNG_WIDTH, PONY_PNG_HEIGHT);
    EVE_cmd_dl(DL_BEGIN | EVE_BITMAPS);
    EVE_cmd_dl(VERTEX2F(logo_x * 16, logo_y * 16));       /* 1/16 pixel units */
    EVE_cmd_dl(DL_END);

    /* tri-bar accent: red / white / blue */
    EVE_cmd_dl(DL_LINE_WIDTH | 16UL);                     /* 1 px corner radius */
    EVE_cmd_dl(DL_BEGIN | EVE_RECTS);
    EVE_color_rgb(COLOR_BAR_R);
    EVE_cmd_dl(VERTEX2F(bars_x * 16, bars_y * 16));
    EVE_cmd_dl(VERTEX2F((bars_x + bar_w) * 16, (bars_y + bar_h) * 16));
    EVE_color_rgb(COLOR_BAR_W);
    EVE_cmd_dl(VERTEX2F((bars_x + bar_w + bar_gap) * 16, bars_y * 16));
    EVE_cmd_dl(VERTEX2F((bars_x + 2 * bar_w + bar_gap) * 16, (bars_y + bar_h) * 16));
    EVE_color_rgb(COLOR_BAR_B);
    EVE_cmd_dl(VERTEX2F((bars_x + 2 * (bar_w + bar_gap)) * 16, bars_y * 16));
    EVE_cmd_dl(VERTEX2F((bars_x + 3 * bar_w + 2 * bar_gap) * 16, (bars_y + bar_h) * 16));
    EVE_cmd_dl(DL_END);

    /* wordmark */
    EVE_color_rgb(COLOR_TITLE);
    EVE_cmd_text((int16_t)(EVE_HSIZE / 2U), 512, 31U, EVE_OPT_CENTER, "MUSTANG");

    EVE_cmd_dl(DL_DISPLAY);                               /* mark end of display list */
    EVE_cmd_dl(CMD_SWAP);                                 /* make the new list active */
    EVE_execute_cmd();                                    /* wait for the co-processor */
}
