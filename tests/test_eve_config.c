/*
 * Invariant test: the vendored EVE library must be configured for exactly the
 * Riverdi SM-RVT70HSBNWN00 panel (7.0" 1024x600 IPS, BT817 / EVE4).
 *
 * These are the things that must absolutely not change without a deliberate
 * hardware decision. In particular this pins the profile-selection rule from
 * docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md:
 * the profile is EVE_RVT70H -- NOT EVE_RiTFT70 (800x480 EVE3) and NOT
 * EVE_RVT70 (800x480 EVE2), whose names read deceptively similar.
 *
 * Host-compiled (EVE_config.h is pure preprocessor, no Arduino deps):
 *   gcc -std=c11 -fsyntax-only -I libraries/FT800-FT813/src tests/test_eve_config.c
 */

#include <stdint.h> /* EVE_config.h's macros cast to uint32_t */
#include "EVE_config.h"

#if !defined(EVE_RVT70H)
#error "EVE_RVT70H must be defined - it selects the 1024x600 BT817 panel profile"
#endif

#if defined(EVE_RiTFT70)
#error "EVE_RiTFT70 must NOT be defined - it is the 800x480 BT815/BT816 (EVE3) profile"
#endif

#if defined(EVE_RVT70)
#error "EVE_RVT70 must NOT be defined - it is the 800x480 FT812/FT813 (EVE2) profile"
#endif

_Static_assert(EVE_HSIZE == 1024UL, "panel width must be 1024 (RVT70H)");
_Static_assert(EVE_VSIZE == 600UL, "panel height must be 600 (RVT70H)");
_Static_assert(EVE_GEN == 4, "chip generation must be EVE4 (BT817)");
_Static_assert(EVE_PCLK_FREQ == 0x0D12UL, "REG_PCLK_FREQ must be 0x0D12 (51 MHz, RVT70H timing)");
_Static_assert(EVE_BACKLIGHT_FREQ == 10000UL,
               "backlight PWM must be 10 kHz (panel datasheet recommends 10-100 kHz; "
               "the library's 4 kHz default whines audibly - bench-verified 2026-07-09)");

int main(void) { return 0; }
