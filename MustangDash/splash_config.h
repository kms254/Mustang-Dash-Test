/*
 * splash_config.h - build-time splash theme selection.
 *
 * All three themes are embedded in the firmware (the theme table in
 * MustangDash.ino references every asset set, so none is dead-code
 * eliminated); this define only picks which one plays at boot.
 *
 * To change themes: edit SPLASH_THEME below and rebuild -- no other edits.
 * A -D SPLASH_THEME=SPLASH_THEME_RED build flag also works (guarded define).
 */

#ifndef SPLASH_CONFIG_H
#define SPLASH_CONFIG_H

#define SPLASH_THEME_BLUE      0
#define SPLASH_THEME_RED       1
#define SPLASH_THEME_CHECKERED 2

#if !defined(SPLASH_THEME)
#define SPLASH_THEME SPLASH_THEME_BLUE
#endif

#if (SPLASH_THEME != SPLASH_THEME_BLUE) && (SPLASH_THEME != SPLASH_THEME_RED) && \
    (SPLASH_THEME != SPLASH_THEME_CHECKERED)
#error "SPLASH_THEME must be SPLASH_THEME_BLUE, SPLASH_THEME_RED, or SPLASH_THEME_CHECKERED"
#endif

#endif /* SPLASH_CONFIG_H */
