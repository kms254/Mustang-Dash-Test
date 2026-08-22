/*
 * MustangDash - Riverdi triple-panel EVE4 dash on STM32
 *
 * Displays: center SM-RVT70HSBNWN00 (7.0" 1024x600) + left/right
 *           SM-RVT50HQBNWN00 (5.0" 800x480), all BT817 / EVE4, no touch
 * MCU     : STM32 (NUCLEO-F767ZI mule; H755 carrier)
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
 *   3. inflates the dash fonts into RAM_G (dash_fonts.h) -- all with the
 *      backlight held dark; the panel's QSPI flash is never touched
 *      (2026-07-21 MCU-direct rewrite)
 *   4. plays the 2000 ms animated splash (assets/splash/README.md spec,
 *      timing in splash_timeline.h; theme picked in splash_config.h),
 *      staging the theme's ASTC bitmaps from the firmware-embedded pack
 *      (splash_flash.h) MCU flash -> RAM_G with per-asset readback
 *      spot-checks, rendering from RAM_G (see splash_render.h for why),
 *      lighting the backlight only after the first frame is on screen
 *   5. crossfades ~400 ms into the dash -- TRACK or STREET view fed by the
 *      built-in simulator (dash_sim.h) with serial overrides (dash_serial.h),
 *      alarm takeover on critical conditions, EEPROM-persisted odometer --
 *      the standing display, redrawn continuously from loop()
 *
 * Serial is 115200 8N1. Boot prints a diagnostic banner; after boot the
 * firmware emits nothing except one `ok ...` / `err ...` ack per received
 * command line (see dash_serial.h for the protocol; /dash skill wraps it).
 * One documented exception: `flashwipe really` prints a pre-erase warning
 * line before its ack, because the erase then blocks silently for minutes.
 *
 * Rendering lives in sibling single-TU headers: dash_draw.h (shared
 * primitives), dash_render.h (center TRACK/STREET/alarm), engine_render.h
 * (left 5" ENGINE screen), timing_render.h (right 5" TIMING/ROAD screen),
 * and splash_render.h (RAM_G staging + splash + crossfade).
 * This file keeps setup/loop and the glue: panel selection, EVE frame
 * plumbing, cluster brightness, fonts, serial pump, odometer EEPROM, and
 * the shared state every header reads.
 *
 * Note: every function below is declared before it is used. PlatformIO does
 * its own .ino prototype generation, so this is belt-and-braces now, but it
 * costs nothing and keeps the file readable top-to-bottom.
 */

#include <Arduino.h>
#include <SPI.h>
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
#include "dash_button.h" /* trip/mode button gesture state machine (host-tested); pin + polarity live below */
#include "dash_fonts.h"
#include "dash_panels.h" /* per-panel pins + timings (host-tested); mapped to EVE_panel_t in setup() */
#include "dash_telltales.h" /* 8-lamp warning mask (host-tested); pins + lamp test live below */
#include "dash_ttsweep.h" /* boot/bench telltale sweep timeline (host-tested); clock + trigger live below */
#include "dash_turnsignal.h" /* turn-signal stalk state + flasher (host-tested); pins + clock live below */
#include "dash_calibration.h" /* per-position telltale dim codes (host-tested, plan 2026-07-28-001 U21) */
#include "dash_can_ford.h" /* Ford 0x270-set decoder (pure, host-tested; plan 2026-08-15-001) -- needs dash_data.h above */
#include "dash_can.h" /* FDCAN bring-up (U7) + the Ford-frame RX drain (plan 2026-08-15-001 U6); stubs where absent */

/* telltale sweep clock: pending=true plays the show on the first live lamp
 * frame after boot (setup()'s ALL-on bulb check holds through the splash,
 * then the chase runs as the dash fades in); the serial `tt sweep` command
 * restarts it any time. The timeline itself is dash_ttsweep.h. */
static bool g_ttsweep_pending = true;
static uint32_t g_ttsweep_start = 0U;

#include <Wire.h> /* FM24CL64B I2C FRAM odometer backend (migration plan U6) */

/* Volatile so the compiler cannot fold the table access down to the one
 * compiled theme -- all three theme rows must stay live (plan requirement
 * R5; the flash pack always carries all three asset sets anyway). */
static volatile uint8_t g_theme = SPLASH_THEME;

/* ---- dash state ---- */

/* Fonts occupy RAM_G from address 0 (~285 KB decoded; dash_fonts.h footer
 * has the exact figure). The splash stages the active theme's assets just
 * above them at boot (~281 KB, center panel only; splash_render.h has the
 * rationale) -- together ~566 KB of the 1 MB. */
struct DashFontLoaded
{
    uint32_t metrics_addr; /* 148-byte block, passed to CMD_SETFONT2 (same layout on every panel's RAM_G) */
    uint8_t ok_mask;       /* bit per panel; a clear bit -> ROM font 31 fallback on that panel only */
};

static DashFontLoaded g_fonts[DASH_FONT_COUNT];
static uint32_t g_ramg_fonts_end = 0UL; /* first free RAM_G byte above the fonts (splash staging base) */
static DashState g_dash;
static DashSimState g_sim;
static DashOdo g_odo;
/* Lap-crossing delta override for the TRACK LAP TIME readout. The state
 * machine is pure (dash_math.h) and host-tested; all that lives here is the
 * once-per-frame tick that feeds it the published channels plus the
 * simulator's sticky per-lap taint. */
static DashLapFlash g_lap_flash;
/* Ford CAN dialect freshness state (plan 2026-08-15-001 U6). Channel values
 * and ownership live in g_dash (can_owned); this holds only per-message
 * last-seen bookkeeping. Static zero-init IS its boot contract ({0}, like
 * dash_state_init). */
static DashCanFord g_can_ford;

/* void* shims adapting the pure Ford decoder to dash_can.h's dialect table.
 * The seam keeps dash_can.h dialect-blind; a second dialect (RaceCapture)
 * registers the same way -- its own state struct, its own pair of shims. */
static bool can_ford_decode_shim(void *state, uint32_t id, uint8_t dlc,
                                 const uint8_t *bytes, uint32_t now_ms,
                                 DashState *s)
{
    return dash_can_ford_decode((DashCanFord *) state, id, dlc, bytes, now_ms, s);
}
static void can_ford_expire_shim(void *state, uint32_t now_ms,
                                 bool car_semantics, DashState *s)
{
    dash_can_ford_expire((DashCanFord *) state, now_ms, car_semantics, s);
}

/* ---- panel plumbing (three BT817s on one shared SPI bus, KTD1/KTD9) ---- */
static EVE_panel_t g_eve_panels[DASH_PANEL_COUNT]; /* library form of DASH_PANELS, filled in setup() */

#if defined(EVE_PANEL_HAS_BUS)
#if defined(DASH_BOARD_NUCLEO_F767)
/* NUCLEO-F767ZI three-panel mule: three genuinely dedicated SPI peripherals
 * on pins clear of the board's fixed functions -- Ethernet RMII steals
 * SPI1's default MOSI (PA7) and SPI2's default SCK (PB13), hence the
 * alternates. Panels attach via FFC breakouts; backlights on external 5V.
 * Constructor order: MOSI, MISO, SCLK. */
static SPIClass g_spi_center(PB5, PA6, PA5);  /* SPI1: MOSI on PB5 (PA7 is RMII) */
static SPIClass g_spi_left(PB15, PC2, PB10);  /* SPI2: SCK on PB10, MISO on PC2 (PB13/PB14 taken) */
static SPIClass g_spi_right(PE6, PE5, PE2);   /* SPI4 */
static SPIClass *const DASH_SPI_BUSES[DASH_PANEL_COUNT] = { &g_spi_center, &g_spi_left, &g_spi_right };
static const uint8_t DASH_CS_PINS[DASH_PANEL_COUNT] = { PF13, PE9, PE11 };
static const uint8_t DASH_PD_PINS[DASH_PANEL_COUNT] = { PF14, PE13, PF15 };
#elif defined(DASH_BOARD_RIVERDI_F469)
/* Riverdi STM32 Evaluation Board (STM32F469II): ONE RiBUS connector on SPI2
 * -- center panel only; the sides are physically absent and retire at boot
 * (R9), so all three descriptors share the one bus and the side CS/PD pins
 * are harmless spares. Pin map from riverdi-eve host_layer/stm32f4:
 * CS PB12, PD PH6, INT PH7 (unused -- we poll). */
static SPIClass g_spi_center(PB15, PB14, PB13); /* SPI2: MOSI, MISO, SCLK */
static SPIClass *const DASH_SPI_BUSES[DASH_PANEL_COUNT] = { &g_spi_center, &g_spi_center, &g_spi_center };
static const uint8_t DASH_CS_PINS[DASH_PANEL_COUNT] = { PB12, PC6, PC7 }; /* sides: spare GPIOs */
static const uint8_t DASH_PD_PINS[DASH_PANEL_COUNT] = { PH6, PC8, PC9 };
#else
/* STM32 carrier (migration plan U5): one dedicated SPI peripheral per panel,
 * indexed by DashPanelDesc.bus_index. Pin sets are compile-valid LQFP-100
 * defaults for the WeAct H743 mule; the carrier schematic (plan U2) owns the
 * final assignment. Constructor order: MOSI, MISO, SCLK. */
static SPIClass g_spi_center(PA7, PA6, PA5); /* SPI1 -- review fix: the variant's
    default `SPI` object IS SPI2 on PB13/14/15, i.e. the same bus as the left
    panel; an explicit SPI1 instance keeps all three buses genuinely disjoint */
static SPIClass g_spi_left(PB15, PB14, PB13); /* SPI2 */
static SPIClass g_spi_right(PE6, PE5, PE2);   /* SPI4 */
static SPIClass *const DASH_SPI_BUSES[DASH_PANEL_COUNT] = { &g_spi_center, &g_spi_left, &g_spi_right };

/* Review fix: dash_panels.h's CS/PD values are canonical digital numbers; on the
 * STM32 variant those same numbers land on USB DP, the telltale port, and the
 * uSD pins. The descriptor table stays host-pure (canonical data); the
 * STM32 build remaps the roles here, clear of USB (PA11/12), lamps (PD0-7),
 * FDCAN (PB5/6/8/9), I2C2 (PB10/11), K1 (PC13), and the SPI pins above.
 * The carrier schematic now owns the assignment (docs/hardware/
 * board3-h755-pin-map.md): right CS moved PD10 -> PE3 at layout (U18,
 * 2026-07-28) so the net escapes the east edge beside its own SPI bus
 * instead of crossing the whole package. Lamps left the MCU entirely in
 * the I2C revision (plan 2026-07-28-001): PD0-PD7 are freed, the
 * telltales ride two AW9523B expanders on I2C2. */
static const uint8_t DASH_CS_PINS[DASH_PANEL_COUNT] = { PD8, PD9, PE3 };
static const uint8_t DASH_PD_PINS[DASH_PANEL_COUNT] = { PD11, PD12, PD13 };
#endif
#endif
static bool g_panel_ok[DASH_PANEL_COUNT];          /* init succeeded; a dead panel stays dark, never blocks the others (R9) */
static uint8_t g_active_panel = DASH_PANEL_CENTER; /* which panel the EVE library is currently routed at */
static uint8_t g_dash_brightness = 0U;             /* ONE cluster brightness (R12); set to BL_STEADY at boot_complete */

/* ---- telltales + switches (migration plan U6) ---- */
/* Pins follow the pin-budget note in dash_panels.h (telltales 2-9,
 * buttons from 24); STM32 pins are WeAct-mule-valid placeholders until the
 * carrier schematic (plan U2) fixes them. */
#if defined(DASH_BOARD_NUCLEO_F767)
/* TEMPORARY bench visibility (2026-07-21): lamps 0-2 drive the Nucleo's
 * onboard LEDs -- LD1 green PB0, LD2 blue PB7, LD3 red PB14 -- so the board
 * visibly participates (boot lamp-test lights all three, then live alarm
 * states) before real telltale hardware exists. Revert to plain GPIOs when
 * external lamps arrive. Remaining lamps on free PD pins (clear of the VCP
 * on PD8/9 and every SPI leg); trip switch = the blue USER button B1. */
static const uint8_t DASH_LAMP_PINS[DASH_TT_COUNT] = { PB0, PB7, PB14, PD0, PD1, PD2, PD3, PD4 };
static const uint8_t DASH_SWITCH_TRIP_PIN = PC13; /* Nucleo USER button B1 */
/* B1 is ACTIVE-HIGH (pressed connects PC13 to VDD; the board carries its
 * own pull-down) -- plain INPUT, and never the internal pull-up, which
 * would fight the external pull-down to an indeterminate idle level
 * (review finding). */
#define DASH_SWITCH_TRIP_PINMODE INPUT
#define DASH_SWITCH_TRIP_PRESSED HIGH
#elif defined(DASH_BOARD_BOARD3)
/* Board3 proper: telltales on the two AW9523B expanders over I2C2 (plan
 * 2026-07-28-001 U21). The gesture button is BTN1 = SW1 on PC6 (netlist:
 * PC6 -> R28 1k -> SW1 -> GND, so the default active-LOW/INPUT_PULLUP
 * contract below is electrically correct). PC13 is NOT connected on
 * Board3 -- the WeAct/Nucleo value would idle high and never fire.
 * BTN2-4 (PC7/PC8/PC9) are wired and unassigned; future gestures land
 * there, not on new hardware. */
#define DASH_LAMPS_I2C 1
static const uint8_t DASH_SWITCH_TRIP_PIN = PC6; /* BTN1 = SW1 */
/* BTN2/BTN4 stand in for the turn-signal stalk on the bench; in the car both
 * blinkers arrive as body signals over CAN. Same electrical contract as BTN1
 * (1k series -> switch -> GND, active-LOW on the internal pull-up). BTN3
 * (PC8) stays unassigned. */
#define DASH_TURN_BUTTONS 1
static const uint8_t DASH_TURN_L_PIN = PC7; /* BTN2 = left */
static const uint8_t DASH_TURN_R_PIN = PC9; /* BTN4 = right */
#else
/* Board3 carrier: the telltales left the MCU -- two AW9523B expanders on
 * I2C2 drive them (plan 2026-07-28-001 U21) and PD0-PD7 are freed. Lamp
 * bit l still drives TT(l+1); device/register tables live in the lamp
 * glue below. Buttons stay on GPIO (KTD20). */
#define DASH_LAMPS_I2C 1
static const uint8_t DASH_SWITCH_TRIP_PIN = PC13; /* WeAct user button K1 */
#endif
/* default trip-switch electrical contract: active-LOW on internal pull-up */
#if !defined(DASH_SWITCH_TRIP_PINMODE)
#define DASH_SWITCH_TRIP_PINMODE INPUT_PULLUP
#define DASH_SWITCH_TRIP_PRESSED LOW
#endif

/* DASH_CAN_CAR (plan 2026-08-15-001 KTD7): opt-in car semantics for CAN
 * staleness, orthogonal-modifier idiom like DASH_MULE_H755Q. Absent (every
 * env today) = bench: a Ford frame stale past 500 ms releases its channels
 * and the simulator reclaims them (R8). Defined = car: stale channels
 * dead-front (`--`) and stay CAN-latched until a fresh frame (R7). Get this
 * wrong in the car and a quiet bus hands the glass back to the simulator --
 * sim fiction rendered as live vitals. Host tests never see this flag (they
 * don't compile the .ino); both semantics stay reachable at runtime through
 * dash_can_ford_expire's parameter. */
#if defined(DASH_CAN_CAR)
#define DASH_CAN_CAR_SEMANTICS true
#else
#define DASH_CAN_CAR_SEMANTICS false
#endif /* DASH_CAN_CAR */

#if defined(DASH_BOARD_BOARD3) && defined(HSE_VALUE) && (HSE_VALUE == 25000000UL)
/* ---- Board3 clock tree (25 MHz crystal, not the Nucleo's 8 MHz bypass) ----
 * Gated on HSE_VALUE, not just the board define, so the SAME firmware runs on
 * an H743-class mule (WeAct or NUCLEO-H743ZI) with the variant's stock clock
 * config: an env that omits -D HSE_VALUE=25000000UL gets the working default
 * clocks, and only the real Board3 env activates this override. That is what
 * lets every OTHER line of the board3 target be proven on bench silicon
 * before Board3 exists.
 * The nucleo_h743zi variant's SystemClock_Config assumes the ST-LINK's 8 MHz
 * MCO; Board3 runs a real 25 MHz crystal (X1, C9006). STM32duino declares
 * SystemClock_Config weak, so this override wins at link time.
 * 25 / M5 = 5 MHz -> x N160 = 800 MHz VCO -> /P2 = 400 MHz SYSCLK, which is
 * VOS1-safe (no VOS0 excursion needed for a dash). AXI/AHB 200 MHz, APB 100.
 * USB FS gets its 48 MHz from HSI48 trimmed by CRS against USB SOF -- the
 * standard crystal-independent recipe, so USB enumeration does not depend on
 * this PLL arithmetic being perfect. Supply is LDO-only: Board3 feeds
 * VDDLDO (three pins in the decoupling census) and floats the SMPS pins --
 * configuring the wrong supply here bricks the chip until power-cycle, so
 * these lines are load-bearing, not boilerplate. */
extern "C" void SystemClock_Config(void)
{
    /* LDO-only, written with raw bits (mirror of the mule block below): we
     * compile against H743 headers on an H755 die, and CR3 bit 2 is SCUEN
     * here but SMPSEN in the silicon. H743's
     * HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY) SETS bit 2 (SCUEN write-1
     * convention) -- on the H755 that re-selects the POR-default
     * SMPS-feeds-LDO chain, whose SMPS pins Board3 floats, so ACTVOSRDY
     * never sets. Hit live at Board3 first power-on (2026-08-19): PC pinned
     * in ExitRun0Mode, CR3=0x46, ACTVOSRDY=0. Clear bit 2, set LDOEN, and
     * make this the FIRST supply write after POR (the shared repo variant
     * no-ops ExitRun0Mode for DASH_BOARD_BOARD3). */
    MODIFY_REG(PWR->CR3,
               (PWR_CR3_SCUEN | PWR_CR3_LDOEN | PWR_CR3_BYPASS),
               PWR_CR3_LDOEN);
    while (0U == (PWR->CSR1 & PWR_CSR1_ACTVOSRDY)) { }
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) { }

    RCC_OscInitTypeDef osc = {};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI48;
    osc.HSEState       = RCC_HSE_ON;
    osc.HSI48State     = RCC_HSI48_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = 5U;
    osc.PLL.PLLN       = 160U;
    osc.PLL.PLLP       = 2U;
    osc.PLL.PLLQ       = 4U;   /* 200 MHz PLL1Q for peripherals that pick it */
    osc.PLL.PLLR       = 2U;
    osc.PLL.PLLRGE     = RCC_PLL1VCIRANGE_2; /* 4-8 MHz ref after /M */
    osc.PLL.PLLVCOSEL  = RCC_PLL1VCOWIDE;
    osc.PLL.PLLFRACN   = 0U;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) { while (1) { } }

    RCC_ClkInitTypeDef clk = {};
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_PCLK1 |
                    RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider  = RCC_SYSCLK_DIV1;  /* CM7 400 MHz */
    clk.AHBCLKDivider  = RCC_HCLK_DIV2;    /* AXI/AHB 200 MHz */
    clk.APB1CLKDivider = RCC_APB1_DIV2;    /* 100 MHz */
    clk.APB2CLKDivider = RCC_APB2_DIV2;
    clk.APB3CLKDivider = RCC_APB3_DIV2;
    clk.APB4CLKDivider = RCC_APB4_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) { while (1) { } }

    RCC_PeriphCLKInitTypeDef pclk = {};
    pclk.PeriphClockSelection = RCC_PERIPHCLK_USB;
    pclk.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
    HAL_RCCEx_PeriphCLKConfig(&pclk);

    __HAL_RCC_CRS_CLK_ENABLE();
    RCC_CRSInitTypeDef crs = {};
    crs.Prescaler             = RCC_CRS_SYNC_DIV1;
    crs.Source                = RCC_CRS_SYNC_SOURCE_USB2; /* OTG_FS SOF (PA11/12) */
    crs.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue           = RCC_CRS_RELOADVALUE_DEFAULT;
    crs.ErrorLimitValue       = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
    HAL_RCCEx_CRSConfig(&crs);
}
#elif defined(DASH_BOARD_BOARD3) && defined(DASH_MULE_H755Q)
/* ---- NUCLEO-H755ZI-Q mule clock tree (SMPS! this block is load-bearing) ----
 * The -Q boards power VCORE from the internal SMPS. The nucleo_h743zi
 * variant's stock SystemClock_Config selects PWR_LDO_SUPPLY, and on
 * SMPS-wired silicon that HANGS the chip at the VOSRDY wait -- the board
 * looks bricked until reflashed under reset. So the mule env MUST carry its
 * own override with PWR_DIRECT_SMPS_SUPPLY; without it, flashing the mule
 * would be the first bench "failure" and it would be ours, not the board's.
 * Clocks: Nucleo HSE is the ST-LINK's 8 MHz MCO (bypass). 8 /M1 = 8 MHz
 * (VCI range 3) x N100 = 800 MHz /P2 = 400 MHz, VOS1 -- the same operating
 * point as the real Board3 block, so timing-derived behaviour transfers.
 * USB stays on HSI48+CRS: running that recipe here proves it on the same
 * H755 die Board3 carries, which is the point of the mule. */
extern "C" void SystemClock_Config(void)
{
    /* PWR_DIRECT_SMPS_SUPPLY does not exist in the H743 variant's headers --
     * the SMPS supply options are dual-core-only HAL surface. The REGISTER
     * exists on the H755 die we are actually running on: H743's PWR_CR3 bit 2
     * is named SCUEN, and on the H755 that same bit position is SDEN (SMPS
     * enable). Direct SMPS = SDEN set, LDOEN and BYPASS clear -- written raw,
     * then wait for the supply-ready flag before touching VOS. This is the
     * one place the H743-variant-on-H755 disguise leaks; everything else is
     * plain shared-die HAL. */
    MODIFY_REG(PWR->CR3,
               (PWR_CR3_SCUEN | PWR_CR3_LDOEN | PWR_CR3_BYPASS),
               PWR_CR3_SCUEN);
    while (0U == (PWR->CSR1 & PWR_CSR1_ACTVOSRDY)) { }
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) { }

    RCC_OscInitTypeDef osc = {};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI48;
    osc.HSEState       = RCC_HSE_BYPASS; /* ST-LINK MCO, not a crystal */
    osc.HSI48State     = RCC_HSI48_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = 1U;
    osc.PLL.PLLN       = 100U;
    osc.PLL.PLLP       = 2U;
    osc.PLL.PLLQ       = 4U;
    osc.PLL.PLLR       = 2U;
    osc.PLL.PLLRGE     = RCC_PLL1VCIRANGE_3; /* 8-16 MHz ref */
    osc.PLL.PLLVCOSEL  = RCC_PLL1VCOWIDE;
    osc.PLL.PLLFRACN   = 0U;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) { while (1) { } }

    RCC_ClkInitTypeDef clk = {};
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_PCLK1 |
                    RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider  = RCC_SYSCLK_DIV1;
    clk.AHBCLKDivider  = RCC_HCLK_DIV2;
    clk.APB1CLKDivider = RCC_APB1_DIV2;
    clk.APB2CLKDivider = RCC_APB2_DIV2;
    clk.APB3CLKDivider = RCC_APB3_DIV2;
    clk.APB4CLKDivider = RCC_APB4_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) { while (1) { } }

    RCC_PeriphCLKInitTypeDef pclk = {};
    pclk.PeriphClockSelection = RCC_PERIPHCLK_USB;
    pclk.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
    HAL_RCCEx_PeriphCLKConfig(&pclk);

    __HAL_RCC_CRS_CLK_ENABLE();
    RCC_CRSInitTypeDef crs = {};
    crs.Prescaler             = RCC_CRS_SYNC_DIV1;
    crs.Source                = RCC_CRS_SYNC_SOURCE_USB2;
    crs.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue           = RCC_CRS_RELOADVALUE_DEFAULT;
    crs.ErrorLimitValue       = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
    HAL_RCCEx_CRSConfig(&crs);
}
#endif /* DASH_BOARD_BOARD3 clock tree */

#if defined(DASH_BOARD_BOARD3) && defined(HAL_QSPI_MODULE_ENABLED)
/* ---- U2 QSPI NOR smoke test (JEDEC ID read, boot banner only) ----
 * U2 (W25Q256JV) has no other firmware consumer yet; the 2026-08-07 decision
 * to keep it fitted was made ON THE CONDITION this read runs on the first
 * board -- until it passes, a dead or miswired U2 is indistinguishable from a
 * working one. Runs once in setup(), prints one banner line, never blocks
 * boot. Pins per the netlist: CLK PB2 (AF9), NCS PG6 (AF10), IO0 PF8 (AF10),
 * IO1 PF9 (AF10), IO2 PF7 (AF9), IO3 PF6 (AF9) -- the H743/H755 QUADSPI AF
 * table; verify against the datasheet at bring-up if the read misbehaves.
 * The ID read is 1-line SPI at kernel/20 (~10 MHz), so quad mode, dummy
 * cycles and flash configuration are all out of scope here on purpose. */
static void board3_qspi_jedec_probe(void)
{
    __HAL_RCC_QSPI_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    GPIO_InitTypeDef g = {};
    g.Mode  = GPIO_MODE_AF_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = GPIO_PIN_2;              g.Alternate = GPIO_AF9_QUADSPI;
    HAL_GPIO_Init(GPIOB, &g);        /* CLK */
    g.Pin = GPIO_PIN_6 | GPIO_PIN_7; g.Alternate = GPIO_AF9_QUADSPI;
    HAL_GPIO_Init(GPIOF, &g);        /* IO3, IO2 */
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9; g.Alternate = GPIO_AF10_QUADSPI;
    HAL_GPIO_Init(GPIOF, &g);        /* IO0, IO1 */
    g.Pin = GPIO_PIN_6;              g.Alternate = GPIO_AF10_QUADSPI;
    HAL_GPIO_Init(GPIOG, &g);        /* NCS */

    QSPI_HandleTypeDef h = {};
    h.Instance                = QUADSPI;
    h.Init.ClockPrescaler     = 19U;  /* kernel clock / 20 -- slow, safe */
    h.Init.FifoThreshold      = 4U;
    h.Init.FlashSize          = 24U;  /* 2^(24+1) = 32 MB = 256 Mbit */
    h.Init.ChipSelectHighTime = QSPI_CS_HIGH_TIME_2_CYCLE;
    h.Init.ClockMode          = QSPI_CLOCK_MODE_0;
    h.Init.FlashID            = QSPI_FLASH_ID_1;
    h.Init.DualFlash          = QSPI_DUALFLASH_DISABLE;
    if (HAL_QSPI_Init(&h) != HAL_OK)
    {
        Serial.println(F("U2 QSPI: HAL init FAILED"));
        return;
    }

    QSPI_CommandTypeDef c = {};
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.Instruction     = 0x9FU; /* JEDEC Read ID */
    c.AddressMode     = QSPI_ADDRESS_NONE;
    c.DataMode        = QSPI_DATA_1_LINE;
    c.NbData          = 3U;
    uint8_t id[3] = { 0U, 0U, 0U };
    if ((HAL_QSPI_Command(&h, &c, 100U) != HAL_OK) ||
        (HAL_QSPI_Receive(&h, id, 100U) != HAL_OK))
    {
        Serial.println(F("U2 QSPI: JEDEC read FAILED (bus dead or U2 missing)"));
        return;
    }
    Serial.printf("U2 QSPI JEDEC: %02X %02X %02X (expect EF 40 19) -- %s\r\n",
                  id[0], id[1], id[2],
                  ((0xEFU == id[0]) && (0x40U == id[1]) && (0x19U == id[2]))
                      ? "ok" : "MISMATCH");
}
#endif /* DASH_BOARD_BOARD3 QSPI probe */

/* ---- lamp drive glue (plan 2026-07-28-001 U21) ----
 * dash_telltales.h stays the only authority on WHICH lamps are lit; these
 * helpers own only how a mask bit reaches its LED. GPIO boards write
 * DASH_LAMP_PINS; the carrier writes AW9523B DIM registers over I2C2. */
#if defined(DASH_LAMPS_I2C)
/* West U11 at 0x5B (AD1=AD0=+5V, every port POR-safe); east U12 at 0x5A
 * (AD1=+5V, AD0=GND -- its POR-safe bank P1_4..P1_7 carries the LEDs, and
 * the west IC uses the same four pins so both sides share one register
 * map). VCC=+5V: a power-on "high" sits at the anode rail, LEDs hard off.
 * AW9523B V2.4 facts the code leans on: ID reg 0x10 reads 0x23; GCR 0x11
 * ISEL=10 caps full-scale at IMAX*2/4 (~18.5 mA) -- inside every LED's
 * 20 mA rating at code 255, with double the code resolution where the
 * calibrated row lives; LED-mode switch 0x13=0x0F puts only P1_4..P1_7 in
 * LED (current-DAC) mode; DIM regs 0x2C..0x2F; the chip wants 5 ms after
 * power before I2C. RSTN is strapped to +5V on the board (internal 100k
 * pull-DOWN would otherwise hold it in reset). */
#define AW_ADDR_WEST   0x5BU
#define AW_ADDR_EAST   0x5AU
#define AW_REG_ID      0x10U
#define AW_ID_VALUE    0x23U
#define AW_REG_GCR     0x11U
#define AW_GCR_ISEL_2Q 0x02U /* ISEL=10 -> 0..IMAX*2/4; D7..5,D3..2 must stay 0 */
#define AW_REG_MODE_P1 0x13U
#define AW_MODE_P1_LED 0x0FU /* P1_7..4 LED mode (0), P1_3..0 GPIO (1) */
/* lamp bit l -> TT(l+1) -> device + DIM register (U19 netlist: TT1/2/5/7
 * west, TT3/4/6/8 east; P1_4..P1_7 = 0x2C..0x2F on both) */
static const uint8_t DASH_LAMP_AW_ADDR[DASH_TT_COUNT] = {
    AW_ADDR_WEST, AW_ADDR_WEST, AW_ADDR_EAST, AW_ADDR_EAST,
    AW_ADDR_WEST, AW_ADDR_EAST, AW_ADDR_WEST, AW_ADDR_EAST
};
static const uint8_t DASH_LAMP_AW_DIM[DASH_TT_COUNT] = {
    0x2CU, 0x2DU, 0x2DU, 0x2CU, 0x2EU, 0x2EU, 0x2FU, 0x2FU
};
static uint8_t g_cal_codes[DASH_CAL_POSITIONS];
/* every position-indexed table must agree on how many lamps exist; the
 * calibration table is authored per position too (dash_calibration.h) */
static_assert(DASH_CAL_POSITIONS == DASH_TT_COUNT,
              "calibration table and lamp positions must be the same length");

static uint16_t g_lamp_code_now[DASH_TT_COUNT]; /* 0x100 = unknown -> force next write */
static bool g_aw_ok[2];                           /* [0] west, [1] east */
static uint8_t g_aw_fail_streak = 0U;
static uint32_t g_aw_last_recover_ms = 0UL;

static bool aw_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return 0U == Wire.endTransmission();
}

static bool aw_config(uint8_t addr)
{
    /* ID first: a wrong or absent chip must not get register writes */
    Wire.beginTransmission(addr);
    Wire.write(AW_REG_ID);
    if (0U != Wire.endTransmission(false))
    {
        return false;
    }
    if ((1U != Wire.requestFrom(addr, (uint8_t)1U)) || (AW_ID_VALUE != (uint8_t)Wire.read()))
    {
        return false;
    }
    return aw_write(addr, AW_REG_GCR, AW_GCR_ISEL_2Q)
           && aw_write(addr, AW_REG_MODE_P1, AW_MODE_P1_LED);
}

static void dash_lamps_init(void)
{
    /* I2C2 pin selection is load-bearing: the variant default collides
     * with FDCAN1 on PB8/PB9 (pin-map known issue #1); the FRAM shares
     * this bus and its later Wire.begin() inherits these pins. */
    Wire.setSDA(PB11);
    Wire.setSCL(PB10);
    Wire.begin();
    while (millis() < 6UL) { } /* AW9523B wants 5 ms after power before I2C */
    dash_cal_codes(g_cal_codes);
    g_aw_ok[0] = aw_config(AW_ADDR_WEST);
    g_aw_ok[1] = aw_config(AW_ADDR_EAST);
    for (uint8_t l = 0U; l < DASH_TT_COUNT; l++)
    {
        g_lamp_code_now[l] = 0x100U;
    }
    Serial.printf("Telltales: AW9523B west %s, east %s\r\n",
                  g_aw_ok[0] ? "ok" : "FAIL", g_aw_ok[1] ? "ok" : "FAIL");
}

static void aw_bus_recover(uint32_t now)
{
    if ((now - g_aw_last_recover_ms) < 1000UL)
    {
        return;
    }
    g_aw_last_recover_ms = now;
    /* KTD19: clock-out-9 frees a slave holding SDA mid-bit, then re-init
     * and reconfigure both devices; shadows reset so every lamp rewrites */
    Wire.end();
    pinMode(PB10, OUTPUT_OPEN_DRAIN);
    for (uint8_t i = 0U; i < 9U; i++)
    {
        digitalWrite(PB10, LOW);
        delayMicroseconds(5);
        digitalWrite(PB10, HIGH);
        delayMicroseconds(5);
    }
    Wire.setSDA(PB11);
    Wire.setSCL(PB10);
    Wire.begin();
    g_aw_ok[0] = aw_config(AW_ADDR_WEST);
    g_aw_ok[1] = aw_config(AW_ADDR_EAST);
    for (uint8_t l = 0U; l < DASH_TT_COUNT; l++)
    {
        g_lamp_code_now[l] = 0x100U;
    }
    g_aw_fail_streak = 0U;
}

static void dash_lamp_set(uint8_t l, bool on)
{
    const uint8_t dev = (DASH_LAMP_AW_ADDR[l] == AW_ADDR_WEST) ? 0U : 1U;
    const uint8_t code = on ? g_cal_codes[l] : 0U;
    if (g_lamp_code_now[l] == (uint16_t)code)
    {
        return; /* 60 fps loop, but writes only on change */
    }
    if (!g_aw_ok[dev])
    {
        return; /* a dead expander stays dark, never blocks the loop (R9's rule) */
    }
    if (aw_write(DASH_LAMP_AW_ADDR[l], DASH_LAMP_AW_DIM[l], code))
    {
        g_lamp_code_now[l] = (uint16_t)code;
        g_aw_fail_streak = 0U;
    }
    else if (++g_aw_fail_streak >= 8U)
    {
        aw_bus_recover(millis());
    }
}
#else
static void dash_lamps_init(void)
{
    for (uint8_t l = 0U; l < DASH_TT_COUNT; l++)
    {
        pinMode(DASH_LAMP_PINS[l], OUTPUT);
    }
}

static void dash_lamp_set(uint8_t l, bool on)
{
    digitalWrite(DASH_LAMP_PINS[l], on ? HIGH : LOW);
}
#endif
/* Gesture state for that one button (U11). Short press toggles TRACK/STREET,
 * long press resets the trip; the debounce + one-fire-per-press latch live in
 * dash_button.h, which is host-tested and polarity-agnostic. */
/* Bench render mode: hold the splash background instead of the dash, so a
 * candidate material can be looked at for longer than the splash lasts. */
static volatile bool g_bg_hold = false;

static DashButton g_trip_btn;
#if defined(DASH_TURN_BUTTONS)
/* Declared here, not beside the sweep globals: DASH_TURN_BUTTONS is set in
 * the board-selection block further up, so a guard placed above it reads as
 * false and the definitions vanish silently. */
static DashTurn g_turn;         /* stalk state (dash_turnsignal.h) */
static DashButton g_turn_l_btn; /* BTN2, debounced like the trip button */
static DashButton g_turn_r_btn; /* BTN4 */
#endif

static char g_serial_line[DASH_SERIAL_MAX_LINE + 17]; /* headroom to detect too-long */
static uint8_t g_serial_len = 0U;

static uint32_t g_loop_last_ms = 0UL;
static uint16_t g_fps = 0U;         /* frames completed in the last full second */
static uint16_t g_fps_frames = 0U;
static uint32_t g_fps_window_ms = 0UL;
static uint16_t g_dl[DASH_PANEL_COUNT][2];          /* boot-measured DL words per panel x {track, street} */
static uint32_t g_eve_faults[DASH_PANEL_COUNT];     /* coprocessor faults auto-recovered by EVE_busy(), per panel */
static uint32_t g_eve_retired[DASH_PANEL_COUNT];    /* frame-drain timeouts that retired the panel (bounded eve_frame_end) */

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
 * conservative 8 MHz; the bus then rises to this once, per panel.
 *
 * BOARD3 OPERATING POINT: 25 MHz, walked and soaked on real copper
 * 2026-08-21. Read the constant, not any prose that quotes a number.
 *
 * The ladder here is COARSE, and it is coarse because this sketch never
 * sets the SPI kernel clocks -- so the H7 reset defaults apply, and they
 * are asymmetric: SPI1/2/3 take PLL1Q (200 MHz), SPI4/5 take APB2
 * (100 MHz). Board3 runs center on SPI1, left on SPI2, right on SPI4.
 * Power-of-two prescalers then land BOTH sources on the same attained
 * rate:
 *     request 13.5 -> center 200/16, left 200/16, right 100/8 = 12.500
 *     request 25.0 -> center 200/8,  left 200/8,  right 100/4 = 25.000
 * That agreement is NOT luck, and it is also NOT guaranteed. It holds
 * because 200:100 is itself a power of two, so the two prescaler ladders
 * are identical from 50 MHz down and agree for EVERY request below
 * 100 MHz. Break that ratio and one constant yields two bus speeds: put
 * SPI123 on a 150 MHz PLL2P with SPI45 still on APB2 100 MHz and a 25 MHz
 * request attains 18.75 on center/left and 25.0 on right -- three panels,
 * two clocks, no compile error and no runtime error. Anything that moves
 * an SPI kernel source must move all three and be re-measured per panel.
 * Available steps are 6.25 / 12.5 / 25 and nothing between, unless the
 * kernel source moves off the defaults.
 *
 * This is also why the REQUEST is not the operating point -- and the
 * trap is subtler than a wrong constant. 13.5 was NOT sloppy when it was
 * written: on the F767, where it was walked and soaked, APB2 is 108 MHz
 * and 108/8 = 13.5 exactly, so it was genuinely attained there. Note the
 * F767 was ALREADY a two-kernel board -- SPI1 off APB2 108 MHz, SPI2 off
 * APB1 54 MHz, and 54/4 = 13.5 as well -- a 2:1 ratio that hid the
 * asymmetry there for the same reason it hides it here. Its ladder was
 * 6.75 / 13.5 / 27 / 54. So 13.5 became
 * a wrong LABEL the moment the same firmware moved to a die with a
 * different clock tree, where 13.5 is not a reachable rung at all. An
 * operating point is a property of constant x clock tree, never of the
 * constant alone; moving silicon invalidates every quantised rate in the
 * tree without changing a line of it. The attained rate is read back from
 * CFG1.MBR and the kernel clock by dash_report_spi_clocks() and printed
 * at boot and on `diag`. Trust that line, not this constant.
 *
 * Evidence for 25 (all three panels live, `diag` + `status` sampled once
 * a minute): 12.5 MHz baseline clean; then at 25 MHz two 20-minute soaks,
 * STREET and TRACK, 20 samples each. 1,920 REG_ID reads, ZERO misses.
 * faults=0,0,0 and retired=0,0,0 throughout, eve=ok,ok,ok, no drift over
 * either leg. fps 60 in TRACK, 57 in STREET -- the cluster's STREET
 * display lists total ~33% more than TRACK (dl 647/689/359 vs 434/434/408:
 * bigger on center and left, slightly SMALLER on right, so quote the total
 * and not the first two), and the reading is steady rather than decaying,
 * so it reads as render cost, not link trouble. NOT PROVEN:
 * there is no 12.5 MHz STREET sample to difference it against.
 *
 * Headroom, and why the walk STOPS here: the BT817 QSPI slave is rated
 * 30 MHz max (Bridgetek datasheet). 25 is 83% of spec. The next rung the
 * prescaler can reach is 50 -- 167% of spec, out of the question -- so
 * the only way up is clock-tree surgery to buy 5 MHz. Not worth it.
 *
 * HISTORY, kept because the failure signature is the reusable part.
 * 24 MHz failed read integrity on the TEENSY LOOM (2026-07-10): flash
 * init 0x01, all 9 font inflates failed GETPTR verification, corrupted
 * REG_CMDB_SPACE reads dragged fps to 25 -- writes mostly survived, reads
 * did not, and fps alone never accepts an operating point. 27 MHz
 * center-only HARD-WEDGED the F767 on jumpers (2026-07-21): serial dead,
 * unbounded busy-poll on corrupted reads. A same-night "three-point clock
 * walk" that blamed SPI crosstalk was confounded by a FLOATING PANEL
 * GROUND. None of those numbers describe Board3 -- they describe a loom
 * and a jumper harness, and Board3 running clean at 25 where the loom
 * broke at 24 is the measurement that retires them. Bench operating
 * points do not transfer across topology; re-walk on any new copper. */
static const uint32_t DASH_SPI_RUN_HZ = 25000000UL; /* attained 25.000 MHz on all three panels */

/* ---- forward declarations (explicit prototypes, see note above) ---- */
void set_backlight(uint8_t duty);
void eve_frame_begin(uint32_t clear_rgb);
void eve_frame_end(void);
bool dash_select_panel(uint8_t idx);
void dash_set_brightness(uint8_t duty);
struct ThemeDesc; /* splash_render.h, included below -- only the pointer is needed here */
void dash_sides_frame(uint8_t alpha, const ThemeDesc *splash_bg, uint8_t bg_alpha,
                      uint32_t clear_rgb);

/* ---- attained SPI clock, read back from the hardware ----------------
 * DASH_SPI_RUN_HZ is a REQUEST. The peripheral rounds it DOWN to a
 * power-of-two prescaler off a kernel clock this sketch never sets
 * explicitly, so the H7 reset defaults apply -- and they are asymmetric:
 * SPI1/2/3 take PLL1Q, SPI4/5 take APB2. Board3 drives center on SPI1,
 * left on SPI2 and right on SPI4, so two different kernel sources are in
 * play and there is no reason a priori for all three panels to land on the
 * same number. Printing the measured figure per panel is what turns a
 * clock walk into a measurement instead of an intention -- the same
 * distinction that made "13.5 MHz" a request nobody had ever confirmed.
 * Reads CFG1.MBR straight out of the peripheral after the raise. */
static void dash_report_spi_clocks(void)
{
#if defined(EVE_PANEL_HAS_BUS) && defined(SPI_CFG1_MBR_Pos)
    static const char *const kNames[3] = { "center", "left", "right" };
    for (uint8_t b = 0U; b < DASH_PANEL_COUNT; b++)
    {
        SPI_TypeDef *inst = DASH_SPI_BUSES[b]->getHandle()->Instance;
        uint32_t ker;
#if defined(RCC_PERIPHCLK_SPI123)
        if (inst == SPI1 || inst == SPI2 || inst == SPI3)
        {
            ker = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI123);
        }
        else if (inst == SPI4 || inst == SPI5)
        {
            ker = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI45);
        }
        else
        {
            ker = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI6);
        }
#else
        ker = HAL_RCC_GetPCLK2Freq();
#endif
        const uint32_t mbr = (inst->CFG1 & SPI_CFG1_MBR_Msk) >> SPI_CFG1_MBR_Pos;
        const uint32_t div = 1UL << (mbr + 1UL);
        Serial.printf("  SPI %-6s kernel %lu Hz / %lu = %.3f MHz attained\r\n",
                      kNames[b < 3U ? b : 2U],
                      (unsigned long) ker, (unsigned long) div,
                      (double) ker / (double) div / 1000000.0);
    }
#else
    Serial.printf("  SPI attained rate: not reported on this target\r\n");
#endif
}
void draw_side_content(uint8_t panel, DashMode mode, uint8_t alpha);
bool load_dash_fonts(uint8_t panel);
uint16_t dash_font(uint8_t idx);
void dash_register_fonts(uint16_t needed);
uint16_t measure_side_dl(uint8_t panel, DashMode mode);
static void odo_storage_init(void); /* review fix: offline no-ctags build needs the explicit prototype */
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
    /* STM32 CDC exposes no cheap "enumerated but monitor closed" signal, so
     * a single bounded wait covers both cases: host present and opening the
     * monitor -> banner captured; car supply -> exits at 500 ms. Bench UX is
     * slightly worse than a native-USB path (a slow-opening monitor can miss
     * the banner); revisit in U6 if it bites. */
    while (!Serial && ((millis() - t_start) < 500U))
    {
        /* wait briefly for a USB host */
    }

    Serial.println();
    Serial.println(F("=== MustangDash / Riverdi triple dash (BT817 x3) on STM32 ==="));
    static const char *const kPanelNames[DASH_PANEL_COUNT] = { "CENTER", "LEFT", "RIGHT" };
    Serial.printf("Splash theme: %u (0=blue; red/checkered retired 2026-08-21)\r\n", (unsigned)g_theme);
#if defined(DASH_BOARD_BOARD3) && defined(HAL_QSPI_MODULE_ENABLED)
    board3_qspi_jedec_probe(); /* U2 smoke test -- the keep-fitted condition */
#endif

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
#if defined(EVE_PANEL_HAS_BUS)
        e->bus = DASH_SPI_BUSES[d->bus_index]; /* dedicated peripheral per panel */
        /* review fix: remap CS/PD off the raw canonical numbers (see the pin
         * tables above); the descriptor stays canonical, the target adapts */
        e->cs_pin = DASH_CS_PINS[p];
        e->pdn_pin = DASH_PD_PINS[p];
#endif
        pinMode(e->cs_pin, OUTPUT);
        digitalWrite(e->cs_pin, HIGH);
        pinMode(e->pdn_pin, OUTPUT);
        digitalWrite(e->pdn_pin, LOW);
        Serial.printf("Panel %s: CS=%u PD=%u %ux%u\r\n", kPanelNames[p],
                      (unsigned)e->cs_pin, (unsigned)e->pdn_pin,
                      (unsigned)d->width, (unsigned)d->height);
    }

    /* SPI mode 0, MSB first; the clock stays conservative through every
     * panel's init (BT817 needs <= 11 MHz until configured), then rises
     * once to the operating point. */
#if defined(EVE_PANEL_HAS_BUS)
    for (uint8_t b = 0U; b < DASH_PANEL_COUNT; b++)
    {
        DASH_SPI_BUSES[b]->begin();
        DASH_SPI_BUSES[b]->beginTransaction(SPISettings(8UL * 1000000UL, MSBFIRST, SPI_MODE0));
    }
    /* "requested", deliberately: 8 MHz rounds DOWN to 6.25 on every one of
     * these buses. A banner that prints a request must say so, or it is the
     * next wrong label -- see the raise line below and the readback. */
    Serial.println(F("3x dedicated SPI up at 8 MHz requested, mode 0 (init)."));
#else
    SPI.begin();
    SPI.beginTransaction(SPISettings(8UL * 1000000UL, MSBFIRST, SPI_MODE0));
    Serial.println(F("SPI up at 8 MHz requested, mode 0 (init)."));
#endif

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
    const bool any_panel_ok = g_panel_ok[0] || g_panel_ok[1] || g_panel_ok[2];

    /* One raise after every init is done (KTD8). Per-panel buses raise
     * independently -- point-to-point links, so per-panel clocks are
     * individually tunable on the STM32 carrier. */
#if defined(EVE_PANEL_HAS_BUS)
    for (uint8_t b = 0U; b < DASH_PANEL_COUNT; b++)
    {
        DASH_SPI_BUSES[b]->endTransaction();
        DASH_SPI_BUSES[b]->beginTransaction(SPISettings(DASH_SPI_RUN_HZ, MSBFIRST, SPI_MODE0));
    }
#else
    SPI.endTransaction();
    SPI.beginTransaction(SPISettings(DASH_SPI_RUN_HZ, MSBFIRST, SPI_MODE0));
#endif
    Serial.printf("SPI raised to %.2f MHz requested (prescaler rounds down; read-integrity soak gates the attained operating point)\r\n",
                  (double)DASH_SPI_RUN_HZ / 1000000.0);
    dash_report_spi_clocks();

    /* Odometer loads regardless of panel state -- it is MCU-local. */
    dash_state_init(&g_dash);
    dash_sim_init(&g_sim);
    dash_lap_flash_reset(&g_lap_flash);
    dash_odo_init(&g_odo);
    /* telltales: drive init + full-row bulb check; the lamps hold ALL
     * through the splash (a visible ~2.4 s lamp test) until loop()'s first
     * live update. On the carrier the bulb check runs at the calibrated
     * codes, so it is also a first look at the matched row. Trip/mode
     * switch: board-specific pin mode (pull-up on most boards, plain INPUT
     * on the Nucleo's active-HIGH B1), debounced and gesture-decoded in
     * loop() via dash_button.h. */
    dash_lamps_init();
    for (uint8_t l = 0U; l < DASH_TT_COUNT; l++)
    {
        dash_lamp_set(l, true);
    }
    pinMode(DASH_SWITCH_TRIP_PIN, DASH_SWITCH_TRIP_PINMODE);
    dash_button_init(&g_trip_btn);
#if defined(DASH_TURN_BUTTONS)
    pinMode(DASH_TURN_L_PIN, DASH_SWITCH_TRIP_PINMODE);
    pinMode(DASH_TURN_R_PIN, DASH_SWITCH_TRIP_PINMODE);
    dash_button_init(&g_turn_l_btn);
    dash_button_init(&g_turn_r_btn);
    dash_turn_init(&g_turn);
#endif

    odo_storage_init();
    odo_eeprom_load();
    dash_can_init(); /* logged, never fatal -- the dash must not depend on CAN (U7) */
    Serial.printf("Odometer: %.1f mi (trip %.1f)\r\n",
                  (double)dash_odo_miles(&g_odo), (double)dash_trip_miles(&g_odo));

    if (any_panel_ok)
    {
        /* Splash assets ship embedded in the firmware and stage into the
         * center panel's RAM_G just before the splash runs (2026-07-21
         * MCU-direct rewrite) -- boot never touches the panel's QSPI flash.
         * Splash needs only a healthy center panel. */
        const bool splash_ok = g_panel_ok[DASH_PANEL_CENTER];

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
        if (splash_ok)
        {
            const ThemeDesc *theme = &THEMES[g_theme];
            /* Stage MCU flash -> RAM_G per panel (bulk SPI writes, 16-byte
             * readback spot-check per asset) -- see the rationale in
             * splash_render.h. Each panel has its own RAM_G, so each needs
             * its own copy of the background; only the centre gets the
             * artwork drawn on top. An asset that fails its check is skipped
             * on that panel alone. */
            for (uint8_t p = 0U; p < DASH_PANEL_COUNT; p++)
            {
                if (g_panel_ok[p] && dash_select_panel(p))
                {
                    (void)splash_stage_theme_to_ramg(theme, p);
                }
            }
            if (dash_select_panel(DASH_PANEL_CENTER))
            {
                run_splash(theme); /* 2000 ms animation, then the crossfade fades the sides in too (R8) */
            }
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

    /* the Ford dialect is the table's only entry today; the return is a
     * boot-time programming-error check, not a runtime condition */
    if (!dash_can_register_dialect(&g_can_ford, can_ford_decode_shim,
                                   can_ford_expire_shim))
    {
        Serial.println(F("CAN: dialect registration FAILED (table full?)"));
    }
    /* discard the ~2.4 s splash-era CAN backlog: stale frames must not decode as fresh, boot RF0L must not count */
    dash_can_rx_flush();
    g_loop_last_ms = millis();
    g_fps_window_ms = g_loop_last_ms;
}

void loop(void)
{
    /* The live pipeline (KTD8): serial -> CAN decode -> sim -> odometer ->
     * alarm -> frame. Runs even when every panel is dead so the bench control
     * surface survives -- only the render step is gated. */
    pump_serial();

    const uint32_t now = millis();
    const uint32_t dt = now - g_loop_last_ms;
    g_loop_last_ms = now;

    /* CAN decode before the sim step (plan 2026-08-15-001 KTD5): accepted
     * frames claim channels via can_owned, so the sim yields to CAN-fresh
     * data on this same frame; the expiry pass then applies the bench/car
     * staleness semantics (KTD6) before the sim gets its turn. `now` is
     * fresh from just after pump_serial, so frame timestamps and dt share
     * one clock reading. */
    dash_can_rx_drain(now, &g_dash);
    dash_can_expire_all(now, DASH_CAN_CAR_SEMANTICS, &g_dash);

    dash_sim_step(&g_sim, &g_dash, dt); /* honors sim_frozen + overrides */

    /* Straight after the sim step, so a lap that closed on THIS step is seen
     * on the frame that renders it. g_sim.last_lap_tainted is the taint of the
     * lap now sitting in LAST -- lap_tainted itself has already been cleared
     * for the new lap by the time we get here, which is the whole reason the
     * sticky copy exists. */
    dash_lap_flash_update(&g_lap_flash, &g_dash, now, g_sim.last_lap_tainted);

    if (dash_ch_valid(&g_dash, DASH_CH_SPEED))
    {
        dash_odo_advance(&g_odo, g_dash.ch.speed_mph, dt);
    }
    if (dash_odo_should_write(&g_odo))
    {
        odo_eeprom_write();
        dash_odo_mark_written(&g_odo);
    }

#if defined(DASH_TURN_BUTTONS)
    /* Turn-signal stalk (BTN2/BTN4). Same debounce contract as the trip
     * button -- a press toggles its side, the other side cancels. The
     * flasher's lit positions are written into tt_signals, the same field a
     * CAN body-signal producer will own: this clears only the two blinker
     * bits, so it never disturbs another signal. */
    {
        const bool l_down = (DASH_SWITCH_TRIP_PRESSED == digitalRead(DASH_TURN_L_PIN));
        const bool r_down = (DASH_SWITCH_TRIP_PRESSED == digitalRead(DASH_TURN_R_PIN));
        if (DASH_BTN_EVENT_SHORT == dash_button_step(&g_turn_l_btn, l_down, now))
        {
            dash_turn_press(&g_turn, DASH_TURN_LEFT, now);
        }
        if (DASH_BTN_EVENT_SHORT == dash_button_step(&g_turn_r_btn, r_down, now))
        {
            dash_turn_press(&g_turn, DASH_TURN_RIGHT, now);
        }
        g_dash.tt_signals = (uint8_t) ((g_dash.tt_signals & ~DASH_TURN_LAMP_BITS)
                                       | dash_turn_mask(&g_turn, now));
    }
#endif

    /* telltales track the live state every frame (U6); the mask is pure
     * logic (dash_telltales.h), only the pin writes live here */
    {
        uint8_t lamp_mask = dash_telltale_mask(&g_dash);
        /* sweep overlay: OR, never mask (same rule as tt_forced) -- an
         * alarm lamp cannot blink off mid-show. Armed at boot; re-armed by
         * the serial `tt sweep` command. */
        if (g_ttsweep_pending)
        {
            g_ttsweep_pending = false;
            g_ttsweep_start = now;
        }
        if (!dash_ttsweep_done(now - g_ttsweep_start))
        {
            lamp_mask |= dash_ttsweep_mask(now - g_ttsweep_start);
        }
        uint8_t first_live = 0U;
#if defined(DASH_BOARD_NUCLEO_F767)
        /* TEMPORARY bench boot show (2026-07-21): the three onboard LEDs
         * hold all-on (continuing setup()'s lamp test through the splash),
         * then roll one-at-a-time green->blue->red every 750 ms until 10 s
         * after boot, then join live telltale duty. Remove together with
         * the onboard-LED pin mapping when real telltale hardware lands. */
        if (now < 10000UL)
        {
            first_live = 3U;
            if (now < 2500UL)
            {
                for (uint8_t l = 0U; l < 3U; l++)
                {
                    digitalWrite(DASH_LAMP_PINS[l], HIGH);
                }
            }
            else
            {
                const uint8_t step = (uint8_t)(((now - 2500UL) / 750UL) % 3U);
                for (uint8_t l = 0U; l < 3U; l++)
                {
                    digitalWrite(DASH_LAMP_PINS[l], (l == step) ? HIGH : LOW);
                }
            }
        }
#endif
        for (uint8_t l = first_live; l < DASH_TT_COUNT; l++)
        {
            dash_lamp_set(l, 0U != ((lamp_mask >> l) & 1U));
        }
    }

    /* Trip/mode button (U11). One button, two gestures -- short press (< 1 s,
     * on release) swaps TRACK/STREET, long press (held >= 1 s) resets the
     * trip. The debounce that a review finding put here is intact and now
     * lives in dash_button.h: nothing fires until the raw level has been
     * stable for > 30 ms, so a single EMI glitch on a car harness cannot
     * zero the trip, and exactly one event fires per press. The polarity
     * comparison stays here, where the board-specific macros are; the state
     * machine only ever sees a normalized bool. */
    {
        const bool down = (DASH_SWITCH_TRIP_PRESSED == digitalRead(DASH_SWITCH_TRIP_PIN));
        switch (dash_button_step(&g_trip_btn, down, now))
        {
            case DASH_BTN_EVENT_SHORT:
                /* Same single assignment the serial `mode track|street`
                 * command makes (dash_serial.h DASH_CMD_MODE) -- mode is a
                 * plain DashState field, nothing else keys off the change. */
                g_dash.mode = (DASH_MODE_TRACK == g_dash.mode) ? DASH_MODE_STREET
                                                               : DASH_MODE_TRACK;
                break;
            case DASH_BTN_EVENT_LONG:
                dash_odo_trip_reset(&g_odo);
                odo_eeprom_write();
                dash_odo_mark_written(&g_odo);
                break;
            case DASH_BTN_EVENT_NONE:
            default:
                break;
        }
    }

    /* Sequential per-panel render (KTD8): center first (mode or alarm
     * takeover), then both sides (mode content only -- R4). Dead panels are
     * skipped inside the helpers (R9). */
    const bool any_ok = g_panel_ok[DASH_PANEL_CENTER] || g_panel_ok[DASH_PANEL_LEFT]
                     || g_panel_ok[DASH_PANEL_RIGHT];
    if (any_ok)
    {
        if (g_bg_hold)
        {
            /* bench hold (`mat`): the splash background on every panel, with
             * no dash on top, so a candidate material can be judged for
             * longer than the 2 s splash allows */
            const ThemeDesc *theme = &THEMES[g_theme];
            if (dash_select_panel(DASH_PANEL_CENTER))
            {
                eve_frame_begin(0x000000UL);
                EVE_color_rgb(0xFFFFFFUL);
                draw_splash_background(theme, DASH_PANEL_CENTER, 255U);
                eve_frame_end();
            }
            dash_sides_frame(0U, theme, 255U, 0x000000UL);
        }
        else
        {
            if (dash_select_panel(DASH_PANEL_CENTER))
            {
                dash_frame(now);
            }
            dash_sides_frame(255U, nullptr, 0U, COLOR_BG);
        }
        g_fps_frames++;
    }

    /* fps accounting for the status ack: frames completed per full second.
     * The window keeps rolling even with every panel retired so fps decays
     * to 0 instead of freezing at the last healthy second -- a frozen 60
     * masked a retired cluster on the bench (review-of-review finding). */
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
 * recoveries so `status` can surface faults=N (review finding).
 *
 * The wait is BOUNDED (review finding, R9): a panel that dies after boot
 * (FFC loose at speed, wedged chip) never reads REG_CMDB_SPACE == 0xffc
 * again, and an unbounded spin here would freeze all three panels plus
 * the serial pump and odometer writes. A healthy frame drains in < 17 ms;
 * on timeout the panel is retired (g_panel_ok false -> dark, skipped by
 * dash_select_panel) and `status` shows it as eve=-- for diagnosis. */
static const uint32_t EVE_FRAME_DRAIN_TIMEOUT_MS = 250UL;
void eve_frame_end(void)
{
    EVE_cmd_dl(DL_DISPLAY);
    EVE_cmd_dl(CMD_SWAP);
    const uint32_t t0 = millis();
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
        if ((millis() - t0) > EVE_FRAME_DRAIN_TIMEOUT_MS)
        {
            g_panel_ok[g_active_panel] = false;
            g_eve_retired[g_active_panel]++;
            break;
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
/* One frame on both side panels.
 *
 * `splash_bg` is non-null only while the splash owns the screen: the two
 * sides carry the same cluster-wide background the centre does, so the whole
 * dash lights as one surface instead of one lit panel between two black ones.
 * During the crossfade both layers are live at once -- background fading out
 * on bg_alpha, dash side content fading in on alpha -- which is why they have
 * to share a single frame rather than being separate passes. */
void dash_sides_frame(uint8_t alpha, const ThemeDesc *splash_bg, uint8_t bg_alpha,
                      uint32_t clear_rgb)
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

    for (uint8_t p = DASH_PANEL_LEFT; p <= DASH_PANEL_RIGHT; p++)
    {
        if (dash_select_panel(p))
        {
            eve_frame_begin(clear_rgb);
            if ((nullptr != splash_bg) && (0U != bg_alpha))
            {
                EVE_color_rgb(0xFFFFFFUL); /* bitmaps draw untinted */
                draw_splash_background(splash_bg, p, bg_alpha);
            }
            if (0U != alpha)
            {
                draw_side_content(p, g_dash.mode, alpha);
            }
            eve_frame_end();
        }
    }
    (void)dash_select_panel(DASH_PANEL_CENTER);
}

/* One side composition: that screen's fonts registered, then its mode
 * content -- the single LEFT->engine / RIGHT->timing dispatch, shared by
 * the frame loop and the boot DL diagnostic (draw_dash_content's shape).
 * The caller has already selected the panel. */
void draw_side_content(uint8_t panel, DashMode mode, uint8_t alpha)
{
    if (DASH_PANEL_LEFT == panel)
    {
        dash_register_fonts(ENG_FONTS);
        if (DASH_MODE_STREET == mode) { engine_street_screen(alpha); } else { engine_track_screen(alpha); }
    }
    else
    {
        dash_register_fonts(TIMING_FONTS);
        if (DASH_MODE_STREET == mode) { timing_street_screen(alpha); } else { timing_track_screen(alpha); }
    }
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
    draw_side_content(panel, mode, 255U);
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

    g_ramg_fonts_end = addr; /* same layout on every panel; splash stages above this */
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

/* CMD_SETFONT2 emits display-list commands, so every frame re-registers
 * its fonts at its top (~5 words per instance), per panel -- each panel's
 * DL carries its own registrations. `needed` is the calling screen's
 * font-index bitmask (CENTER_FONTS / ENG_FONTS / TIMING_FONTS): instances
 * a screen never references stay out of its display list entirely. */
void dash_register_fonts(uint16_t needed)
{
    for (uint8_t i = 0U; i < DASH_FONT_COUNT; i++)
    {
        if ((0U != ((needed >> i) & 1U))
            && (0U != ((g_fonts[i].ok_mask >> g_active_panel) & 1U)))
        {
            EVE_cmd_setfont2((uint32_t)DASH_FONTS[i].handle,
                             g_fonts[i].metrics_addr,
                             (uint32_t)DASH_FONTS[i].firstchar);
        }
    }
}

/* ---- odometer EEPROM glue (KTD7): the pure record logic lives in
 * dash_odo.h; this is the only code that touches the EEPROM API ---- */
/* Storage seam (unique names -- the .ino->cpp prototype hoisting in both
 * build paths would lift any redefinition of the avr eeprom_* names above
 * target guards and collide with a platform eeprom header). U6 replaces the
 * STM32 branch with the FM24CL64B I2C FRAM backend. */
/* STM32 backend (U6): FM24CL64B I2C FRAM at 0x50, 16-bit addressing, no
 * write-cycle delay (FRAM writes at bus speed -- no polling, no wear
 * leveling). Probed once at boot; a missing chip (e.g. the bare WeAct
 * mule) degrades to a RAM shadow: the dash runs, the odometer just does
 * not persist, and the banner says so. */
static const uint8_t ODO_FRAM_ADDR = 0x50U;
static bool g_odo_fram_ok = false;
static uint8_t g_odo_shadow[DASH_ODO_SLOT_ADDR(1) + DASH_ODO_RECORD_SIZE];

static void odo_storage_init(void)
{
    Wire.begin();
    Wire.beginTransmission(ODO_FRAM_ADDR);
    g_odo_fram_ok = (0U == Wire.endTransmission());
    Serial.printf("Odometer backend: %s\r\n",
                  g_odo_fram_ok ? "FRAM (FM24CL64B)" : "RAM shadow -- NOT persistent (no FRAM found)");
}

static void odo_storage_read(void *dst, uint32_t off, size_t n)
{
    memset(dst, 0, n); /* review fix: a short I2C read must yield a clean
                        * CRC-fail record, never uninitialized stack tails */
    if (!g_odo_fram_ok)
    {
        memcpy(dst, &g_odo_shadow[off], n);
        return;
    }
    Wire.beginTransmission(ODO_FRAM_ADDR);
    Wire.write((uint8_t)(off >> 8));
    Wire.write((uint8_t)(off & 0xFFU));
    const bool addr_ok = (0U == Wire.endTransmission(false)); /* repeated start */
    const size_t got = addr_ok ? (size_t)Wire.requestFrom(ODO_FRAM_ADDR, (uint8_t)n) : 0U;
    if (got != n)
    {
        /* review fix: a transient read fault must NOT zero-start and then
         * clobber the surviving FRAM records on the next cadence write.
         * Latch the shadow backend: this drive runs unpersisted, the good
         * records stay untouched for the next boot's fresh probe. */
        g_odo_fram_ok = false;
        Serial.printf("Odometer FRAM read fault (got %u/%u) -> RAM shadow for this run\r\n",
                      (unsigned)got, (unsigned)n);
        memcpy(dst, &g_odo_shadow[off], n);
        return;
    }
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0U; (i < n) && (Wire.available() > 0); i++)
    {
        p[i] = (uint8_t)Wire.read();
    }
}

static void odo_storage_write(const void *src, uint32_t off, size_t n)
{
    if (!g_odo_fram_ok)
    {
        memcpy(&g_odo_shadow[off], src, n);
        return;
    }
    Wire.beginTransmission(ODO_FRAM_ADDR);
    Wire.write((uint8_t)(off >> 8));
    Wire.write((uint8_t)(off & 0xFFU));
    Wire.write((const uint8_t *)src, n);
    if (0U != Wire.endTransmission()) /* review fix: a lost write must not be silent */
    {
        /* PR#6 review: the record being persisted must still land -- copy
         * it into the RAM shadow (the backend all future calls now use)
         * and log, mirroring the read-fault diagnostic. Without this, one
         * transient I2C fault silently dropped the current record (e.g.
         * the trip-reset's immediate anti-resurrection write). */
        g_odo_fram_ok = false; /* degrade to shadow; reboot re-probes fresh */
        memcpy(&g_odo_shadow[off], src, n);
        Serial.println(F("Odometer FRAM write fault -> RAM shadow for this run"));
    }
}

void odo_eeprom_load(void)
{
    /* Two-slot ping-pong (review finding): a torn write -- power loss
     * mid-write on the 12V rail -- corrupts at most the slot being
     * written; the other slot still holds the previous odometer, so a
     * tear costs 0.1 mi, never the lifetime count. */
    uint8_t rec0[DASH_ODO_RECORD_SIZE];
    uint8_t rec1[DASH_ODO_RECORD_SIZE];
    odo_storage_read(rec0, DASH_ODO_SLOT_ADDR(0), DASH_ODO_RECORD_SIZE);
    odo_storage_read(rec1, DASH_ODO_SLOT_ADDR(1), DASH_ODO_RECORD_SIZE);
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
    odo_storage_write(rec, DASH_ODO_SLOT_ADDR(slot), DASH_ODO_RECORD_SIZE);
}

/* ---- serial pump (KTD6): accumulate a line, parse, apply, ack ---- */
void pump_serial(void)
{
    while (Serial.available() > 0)
    {
        const char c = (char)Serial.read();
        if ('\n' == c)
        {
            /* PR#6 review: trim trailing CR/blanks so CRLF terminals reach
             * the pre-parser commands (cantest) whose raw whole-line
             * compare has no tokenizer to strip them -- every parser-side
             * command already tolerated the \r, only these didn't. */
            while ((g_serial_len > 0U) &&
                   (('\r' == g_serial_line[g_serial_len - 1U]) ||
                    (' ' == g_serial_line[g_serial_len - 1U]) ||
                    ('\t' == g_serial_line[g_serial_len - 1U])))
            {
                g_serial_len--;
            }
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
    /* `cantest` (U7): one-shot loopback proof, bus 1 -> bus 2 with the two
     * buses wire-jumpered. Ahead of the parser like a diagnostic, because
     * it is one: not part of the channel protocol. Case-insensitive to
     * honor the protocol's documented contract (review fix). */
    if (dash_serial_ieq_(line, "cantest"))
    {
        Serial.println(dash_can_test() ? F("ok cantest") : F("err cantest failed (init/jumper/termination/timeout)"));
        return;
    }

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
        case DASH_CMD_CIRCUIT:
            /* The circuit lives in DashSimState, which dash_serial.h sits
             * below and cannot reach -- same reason ODO_SET lands here. One
             * ack and nothing else: the protocol's contract is that ok/err
             * are the only output after boot. */
            dash_sim_set_circuit(&g_sim,
                                 cmd.circuit_sweep ? SIM_CIRCUIT_SWEEP : SIM_CIRCUIT_HPR);
            Serial.printf("ok circuit %s\r\n", cmd.circuit_sweep ? "sweep" : "hpr");
            break;
        case DASH_CMD_BRIGHT: {
            /* cmd.bright is percent 0-100; scale to REG_PWM_DUTY 0-128 (rounded).
             * Hardware path, so it lands here (dash_serial.h can't reach the
             * panels). One ack, echoing the percent the user asked for. */
            uint8_t duty = (uint8_t) (((uint16_t) cmd.bright * 128U + 50U) / 100U);
            dash_set_brightness(duty);
            Serial.printf("ok bright %u%%\r\n", (unsigned) cmd.bright);
            break;
        }
        case DASH_CMD_TT_SWEEP:
            /* the animation clock is millis territory, so it lands here
             * like BRIGHT; the frame lamp block plays the timeline */
            g_ttsweep_pending = false;
            g_ttsweep_start = millis();
            Serial.printf("ok tt sweep\r\n");
            break;
        case DASH_CMD_ODO_SET:
            dash_odo_reseed(&g_odo, cmd.value);
            odo_eeprom_write();
            dash_odo_mark_written(&g_odo);
            Serial.printf("ok odo set %.1f\r\n", (double)dash_odo_miles(&g_odo));
            break;
        case DASH_CMD_FLASHWIPE:
            /* Full-chip erase of the center panel's QSPI flash (plan
             * 2026-07-21-002 U5/KTD7): retires the obsolete ESE image, blob
             * included -- boot never reads panel flash. A 64 MB chip erase
             * takes MINUTES: the dash freezes and serial stays silent until
             * the ok. Basic flash mode suffices for erase (no blob needed). */
            if (g_panel_ok[DASH_PANEL_CENTER] && dash_select_panel(DASH_PANEL_CENTER))
            {
                Serial.println(F("flashwipe: erasing (minutes of silence -- do NOT power-cycle)"));
                /* Clear the fault latch first so the ack attributes any fault
                 * to THIS erase; EVE_busy() auto-recovers coprocessor faults
                 * silently (resets the chip, clears the ring -- the erase
                 * would abort), so an unconditional ok would be a false pass
                 * on the one irreversible command (review finding). */
                (void)EVE_get_and_reset_fault_state();
                /* EVE_init_flash() is the library's full INIT-wait +
                 * attach-retry state walk -- the recipe the old boot flow
                 * used (a bare mid-session CMD_FLASHATTACH left the flash
                 * DETACHED on the bench, and erasing a detached flash
                 * no-ops below the fault latch: the 0-second fake ok,
                 * 2026-07-21). A flashfast error is fine -- erase only
                 * needs BASIC (status >= 2). */
                const uint8_t finit = EVE_init_flash();
                const uint32_t fst_pre = EVE_memRead32(REG_FLASH_STATUS);
                if (fst_pre < 2UL)
                {
                    Serial.printf("err flashwipe flash not attached (init=0x%02X, REG_FLASH_STATUS=%lu)\r\n",
                                  finit, (unsigned long)fst_pre);
                    break;
                }
                {
                    const uint32_t t0 = millis();
                    EVE_cmd_flasherase();
                    EVE_execute_cmd();
                    const uint32_t secs = (millis() - t0) / 1000UL;
                    const uint32_t fst_post = EVE_memRead32(REG_FLASH_STATUS);
                    if (EVE_FAULT_RECOVERED == EVE_get_and_reset_fault_state())
                    {
                        Serial.println(F("err flashwipe coprocessor fault during erase (flash state unknown)"));
                    }
                    else if (secs < 10UL)
                    {
                        /* a real 64 MB chip erase takes minutes; an instant
                         * return means it did not happen */
                        Serial.printf("err flashwipe suspiciously fast (%lus, status=%lu) -- erase likely did not run\r\n",
                                      (unsigned long)secs, (unsigned long)fst_post);
                    }
                    else
                    {
                        Serial.printf("ok flashwipe (%lus, status=%lu)\r\n",
                                      (unsigned long)secs, (unsigned long)fst_post);
                    }
                }
            }
            else
            {
                Serial.println(F("err flashwipe center panel unavailable"));
            }
            break;
        case DASH_CMD_STATUS: {
            /* Full-state snapshot (context parity, review finding): every
             * channel with `--` for invalid -- matching what the panel
             * dead-fronts -- plus the active alarm, sim state, and the
             * auto-recovered coprocessor fault count. One line, one ack. */
            const DashAlarm alarm = dash_alarm_classify(&g_dash);
            Serial.printf("ok status mode=%s fps=%u alarm=%s sim=%s faults=%lu,%lu,%lu retired=%lu,%lu,%lu",
                          (DASH_MODE_TRACK == g_dash.mode) ? "track" : "street",
                          (unsigned)g_fps,
                          (DASH_ALARM_OILP == alarm) ? "oilp" :
                          (DASH_ALARM_OILT == alarm) ? "oilt" :
                          (DASH_ALARM_CLT == alarm) ? "clt" : "none",
                          g_dash.sim_frozen ? "off" : "on",
                          (unsigned long)g_eve_faults[0],
                          (unsigned long)g_eve_faults[1],
                          (unsigned long)g_eve_faults[2],
                          (unsigned long)g_eve_retired[0],
                          (unsigned long)g_eve_retired[1],
                          (unsigned long)g_eve_retired[2]);
            /* Live center-panel register probe (works even on a retired
             * panel -- bypasses dash_select_panel's liveness gate). A
             * post-reset BT817 reads REG_ID 0x7C (ROM) but REG_PCLK 0 and
             * REG_PWM_DUTY 32 (25%): distinguishes "chip reset itself"
             * from "chip wedged with config intact" after a death. */
            (void)EVE_select_panel(&g_eve_panels[DASH_PANEL_CENTER]);
            Serial.printf(" creg=id:0x%02X,pclk:%u,pwm:%u",
                          EVE_memRead8(REG_ID),
                          (unsigned)EVE_memRead8(REG_PCLK),
                          (unsigned)EVE_memRead8(REG_PWM_DUTY));
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
            /* can= is the FDCAN1 decode pulse (KTD11, plan 2026-08-15-001):
             * frames the Ford decoder consumed (filter-admitted strangers
             * and runts don't count), FIFO0 overflow episodes, ms since the
             * last consumed frame, and the active staleness semantics --
             * "bench" (stale releases to the sim, R8) or "car" (stale
             * dead-fronts and stays latched, R7), otherwise unobservable at
             * runtime. lost>0 means "dropping frames", a large ms-since
             * with accepted>0 means "bus went quiet". */
            Serial.printf(" odo=%.1f trip=%.1f dl=%u/%u,%u/%u,%u/%u can=%lu,%lu,%lu,%s eve=%s,%s,%s\r\n",
                          (double)dash_odo_miles(&g_odo), (double)dash_trip_miles(&g_odo),
                          (unsigned)g_dl[0][0], (unsigned)g_dl[0][1],
                          (unsigned)g_dl[1][0], (unsigned)g_dl[1][1],
                          (unsigned)g_dl[2][0], (unsigned)g_dl[2][1],
                          (unsigned long)dash_can_rx_accepted(),
                          (unsigned long)dash_can_rx_lost(),
                          (unsigned long)dash_can_rx_ms_since_accept(millis()),
                          DASH_CAN_CAR_SEMANTICS ? "car" : "bench",
                          g_panel_ok[0] ? "ok" : "--",
                          g_panel_ok[1] ? "ok" : "--",
                          g_panel_ok[2] ? "ok" : "--");
            break;
        }
        case DASH_CMD_MAT:
        /* Hold the splash background on every panel so a machined finish can
         * actually be judged. The 2 s splash is not long enough to look at a
         * surface, and a scaled-down preview misleads -- which is how the
         * first material shipped reading as bubbles. */
        g_bg_hold = cmd.mat_hold;
        Serial.printf("ok mat %s\r\n", cmd.mat_hold ? "on" : "off");
        break;

    case DASH_CMD_DIAG: {
            /* The boot banner races CDC enumeration (500 ms Serial wait), so
             * everything it reports has been effectively unreadable. This
             * reprints the parts that come from hardware, plus the one thing
             * a banner cannot give you: a LIVE read-integrity sample.
             *
             * 16 REG_ID reads per panel, all of which must return 0x7C. This
             * is the gate the clock walk actually turns on -- at 24 MHz on
             * the Teensy wiring, fps and faults both stayed clean while reads
             * were corrupting ("writes mostly survived, reads did not"), so
             * fps alone has already fooled this project once. */
            static const char *const kNames[DASH_PANEL_COUNT] = { "CENTER", "LEFT", "RIGHT" };
            Serial.println(F("ok diag"));
            dash_report_spi_clocks();
            /* Output depth and dither were never configured for this panel --
             * the library sets them only for an unrelated board -- so they sit
             * at reset defaults nobody had ever checked. A dark gradient bands
             * the moment either is wrong, so report them instead of assuming. */
            for (uint8_t p = 0U; p < DASH_PANEL_COUNT; p++)
            {
                if (g_panel_ok[p] && dash_select_panel(p))
                {
                    Serial.printf("  panel %-6s REG_OUTBITS 0x%03X REG_DITHER %u\r\n",
                                  kNames[p],
                                  (unsigned)EVE_memRead16(REG_OUTBITS),
                                  (unsigned)EVE_memRead8(REG_DITHER));
                }
            }
            (void)dash_select_panel(DASH_PANEL_CENTER);
#if defined(DASH_BOARD_BOARD3) && defined(HAL_QSPI_MODULE_ENABLED)
            board3_qspi_jedec_probe();
#endif
            for (uint8_t p = 0U; p < DASH_PANEL_COUNT; p++)
            {
                if (!g_panel_ok[p])
                {
                    Serial.printf("  panel %-6s retired/absent\r\n", kNames[p]);
                    continue;
                }
                uint8_t good = 0U;
                uint8_t last = 0xFFU;
                for (uint8_t i = 0U; i < 16U; i++)
                {
                    if (!dash_select_panel(p)) { break; }
                    last = EVE_memRead8(REG_ID);
                    if (last == 0x7CU) { good++; }
                }
                Serial.printf("  panel %-6s REG_ID %u/16 good (last 0x%02X)\r\n",
                              kNames[p], (unsigned) good, (unsigned) last);
            }
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
