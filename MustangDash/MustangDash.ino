/*
 * MustangDash - first light for a Riverdi 7" EVE4 panel on a Teensy 4.1
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
 *   3. writes REG_PWM_DUTY = 128 to turn the backlight fully on (the PWM
 *      scale is 0..128, so 128 = 100%; the library's own default is 0x20 = 25%)
 *   4. draws one display list: dark background, "HELLO MUSTANG" centered in a
 *      large ROM font with a smaller "EVE4 first light" line under it
 *   5. in loop() slowly pulses REG_PWM_DUTY between 20 and 128 so the render
 *      loop and software dimming are visibly working
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
#include "backlight_wave.h"

/* ---- forward declarations (explicit prototypes, see note above) ---- */
void draw_first_light(void);
void set_backlight(uint8_t duty);

/* colours (0xRRGGBB) */
static const uint32_t COLOR_BG       = 0x0A0E14UL; /* near-black, faint blue */
static const uint32_t COLOR_TITLE    = 0xFFFFFFUL; /* white */
static const uint32_t COLOR_SUBTITLE = 0x8A93A6UL; /* muted grey-blue */

/* backlight pulse limits requested for the loop() demo */
static const uint8_t BL_MIN = 20U;
static const uint8_t BL_MAX = 128U;

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
        set_backlight(BL_MAX);        /* backlight fully on (REG_PWM_DUTY = 128 = 100%) */
        draw_first_light();           /* one static display list */
        Serial.println(F("EVE init OK: display list sent, backlight on. Pulsing now."));
    }
    else
    {
        Serial.println(F("EVE_init() did NOT return E_OK - see failure guide in the summary."));
        Serial.println(F("Halting render; will still print this once. Check wiring / power / SPI."));
    }
}

void loop(void)
{
    static uint8_t duty = BL_MAX;
    static int8_t  step = -2;        /* start by dimming down */

    if (!eve_ready)
    {
        return; /* nothing to pulse if the panel never initialised */
    }

    /* slow triangle wave between BL_MIN and BL_MAX on REG_PWM_DUTY
     * (pure stepping logic lives in backlight_wave.h, host-tested in tests/) */
    duty = bl_wave_next(duty, &step, BL_MIN, BL_MAX);

    set_backlight(duty);
    delay(20);                       /* 108 steps x 20 ms: ~2.2 s per full min->max->min sweep */
}

/* Write the backlight PWM duty (0..128 is the useful range on EVE). */
void set_backlight(uint8_t duty)
{
    EVE_memWrite8(REG_PWM_DUTY, duty);
}

/* Build and swap in a single display list: dark background + two text lines. */
void draw_first_light(void)
{
    EVE_cmd_dl(CMD_DLSTART);                              /* start a new display list */
    EVE_cmd_dl(DL_CLEAR_COLOR_RGB | COLOR_BG);            /* set background colour */
    EVE_cmd_dl(DL_CLEAR | CLR_COL | CLR_STN | CLR_TAG);   /* clear colour/stencil/tag */

    /* title: large ROM font (31 is the largest built-in ROM font on BT817) */
    EVE_color_rgb(COLOR_TITLE);
    EVE_cmd_text((int16_t)(EVE_HSIZE / 2U), (int16_t)((EVE_VSIZE / 2U) - 40),
                 31U, EVE_OPT_CENTER, "HELLO MUSTANG");

    /* subtitle: smaller ROM font, just below the title */
    EVE_color_rgb(COLOR_SUBTITLE);
    EVE_cmd_text((int16_t)(EVE_HSIZE / 2U), (int16_t)((EVE_VSIZE / 2U) + 40),
                 27U, EVE_OPT_CENTER, "EVE4 first light");

    EVE_cmd_dl(DL_DISPLAY);                               /* mark end of display list */
    EVE_cmd_dl(CMD_SWAP);                                 /* make the new list active */
    EVE_execute_cmd();                                    /* wait for the co-processor */
}
