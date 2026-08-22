/*
 * splash_render.h - the boot splash, whole: theme/asset descriptors, the
 * MCU-flash -> RAM_G staging of the embedded ASTC pack, the splash element
 * renderers, and the 2000 ms animation + crossfade sequence. Pure code
 * motion out of MustangDash.ino.
 *
 * NOT a pure header: everything here drives the EVE co-processor, so it is
 * not host-testable and is not part of tests/run-tests.sh. Include exactly
 * once, from MustangDash.ino, AFTER dash_render.h -- run_splash() calls
 * draw_dash_content() during the crossfade -- and after the glue prototypes
 * (set_backlight, eve_frame_begin/end) and shared constants (COLOR_BG,
 * BL_STEADY) that the code below uses.
 */
#ifndef SPLASH_RENDER_H
#define SPLASH_RENDER_H

/* ---- asset plumbing ---- */

/* Splash assets are ASTC bitmaps EMBEDDED in the firmware image (address
 * table + pack in splash_flash.h -- the "flash" naming is the pack's
 * historical address space, base 4096) and staged MCU flash -> RAM_G once
 * at boot (splash_stage_theme_to_ramg); every draw sources RAM_G.
 *
 * History (2026-07-21 MCU-direct rewrite): the pack used to be provisioned
 * into the panel's own QSPI flash, staged flash->RAM_G at boot, with
 * direct-from-flash render as a degraded fallback. All of it is gone.
 * Direct flash render tears above a per-frame bandwidth ceiling every
 * module has (measured 40-56 KB/asset; see docs/solutions/
 * architecture-patterns/bt817-flash-render-streaming-bandwidth-ceiling.md),
 * and the panel-flash round trip added a provisioning step plus a boot
 * dependency on flash init without saving any MCU flash -- the pack ships
 * embedded either way. Boot never touches the panel's flash now.
 *
 * Cost: fonts (~285 KB) and the staged theme are RAM_G co-tenants through
 * the crossfade. Blue/red carry 4x4 backgrounds (theme ~741 KB, peak
 * ~1,026 KB of the center's 1 MiB -- ~22 KB headroom, and blue is the
 * shipping default); checkered stays 6x6 (peak ~706 KB). An asset that
 * fails its staging spot-check is skipped for the session; there is no
 * flash fallback. */

struct ThemeDesc
{
    /* Background, in two layers of very different spatial frequency --
     * tools/make_material.py has the full rationale. The tile is machined
     * metal drawn REPEAT at native resolution; the glow is a tiny slice of
     * one cluster-wide field, stretched BILINEAR. Neither can substitute
     * for the other: upscaling the tile destroys the machining, and ASTC
     * quantises the glow into visible blocks. */
    const SplashFlashAsset *metal;                     /* seamless tile, tiled over the panel */
    const SplashFlashAsset *glow[DASH_PANEL_COUNT];    /* per-panel slice, indexed by panel id */
    const SplashFlashAsset *side;  /* chrome bars, drawn left+right */
    const SplashFlashAsset *line;  /* accent line, revealed from center */
    const SplashFlashAsset *year;  /* "1965" */
    const SplashFlashAsset *strip; /* unused since the checkered theme retired */
    bool has_strip;
    int16_t side_y; /* bars sit at y 202 */
    int16_t line_y; /* line canvas at y 420 */
};

#define SPLASH_FA(idx) (&SPLASH_FLASH_ASSETS[idx])

static const ThemeDesc THEMES[1] = {
    /* SPLASH_THEME_BLUE -- the only theme that ships (splash_config.h) */
    { SPLASH_FA(SPLASH_FA_METAL_TILE),
      { SPLASH_FA(SPLASH_FA_GLOW_CENTER),
        SPLASH_FA(SPLASH_FA_GLOW_LEFT),
        SPLASH_FA(SPLASH_FA_GLOW_RIGHT) },
      SPLASH_FA(SPLASH_FA_BARS_CHROME),
      SPLASH_FA(SPLASH_FA_LINE_BLUE), SPLASH_FA(SPLASH_FA_YEAR_BLUE),
      nullptr, false, SPLASH_BAR_Y, SPLASH_LINE_Y },
};

/* THEMES[] rows are positional: the theme id doubles as the array index.
 * The glow[] rows are ordered by PANEL id, not by physical left-to-right --
 * DASH_PANEL_CENTER is 0 -- so the table reads center, left, right. */
static_assert(SPLASH_THEME_BLUE == 0,
              "THEMES[] is indexed by the SPLASH_THEME_* values");
static_assert((DASH_PANEL_CENTER == 0) && (DASH_PANEL_LEFT == 1) && (DASH_PANEL_RIGHT == 2),
              "ThemeDesc::glow[] is indexed by panel id; reorder it if the panel ids move");

/* ---- RAM_G budget, checked at BUILD time --------------------------------
 *
 * This budget overflowed silently for a month (2026-07-22 to 2026-08-21).
 * The dash fonts grew in an unrelated commit -- DF_MID gained A-Z so a lap
 * flash could spell BEST -- and the splash's last two assets stopped
 * fitting. The only evidence was a boot banner that loses a race with CDC
 * enumeration, so the blue splash played without its accent line or its
 * year on every single boot and nobody saw a thing. The comment describing
 * the budget still said "~285 KB" of fonts, which is what anyone checking
 * on paper would have believed.
 *
 * Every number involved is a compile-time constant, including the decoded
 * font sizes, so the check belongs here rather than in a printf. If this
 * fires, either the fonts grew or an asset did; the fix is to find the
 * space, not to raise the ceiling.
 *
 * Layout mirrors what splash_stage_theme_to_ramg() actually does: fonts
 * from address 0, then the theme 64-byte aligned above them, in staging
 * order. The centre panel is the worst case because it alone carries the
 * artwork on top of the background. */

/* one font instance: 148-byte metric block + inflated glyphs, 4-byte packed */
#define DASH_FONT_RAMG(glyph_bytes) ((148UL + (unsigned long)(glyph_bytes) + 3UL) & ~3UL)
#define DASH_FONTS_RAMG_TOTAL                    \
    (DASH_FONT_RAMG(DASH_FONT_HERO_GLYPH_BYTES)  \
     + DASH_FONT_RAMG(DASH_FONT_BIG_GLYPH_BYTES) \
     + DASH_FONT_RAMG(DASH_FONT_MID_GLYPH_BYTES) \
     + DASH_FONT_RAMG(DASH_FONT_VAL_GLYPH_BYTES) \
     + DASH_FONT_RAMG(DASH_FONT_SMALL_GLYPH_BYTES) \
     + DASH_FONT_RAMG(DASH_FONT_TITLE_GLYPH_BYTES) \
     + DASH_FONT_RAMG(DASH_FONT_LABEL_GLYPH_BYTES) \
     + DASH_FONT_RAMG(DASH_FONT_TINY_GLYPH_BYTES) \
     + DASH_FONT_RAMG(DASH_FONT_LAP_GLYPH_BYTES))

#define SPLASH_STAGE_NEXT(base, size) ((((base) + (unsigned long)(size)) + 63UL) & ~63UL)

/* centre panel: background + every drawn asset, in staging order */
#define SPLASH_RAMG_BASE      ((DASH_FONTS_RAMG_TOTAL + 63UL) & ~63UL)
#define SPLASH_RAMG_C1 SPLASH_STAGE_NEXT(SPLASH_RAMG_BASE, SPLASH_FA_METAL_TILE_SIZE)
#define SPLASH_RAMG_C2 SPLASH_STAGE_NEXT(SPLASH_RAMG_C1, SPLASH_FA_GLOW_CENTER_SIZE)
#define SPLASH_RAMG_C3 SPLASH_STAGE_NEXT(SPLASH_RAMG_C2, SPLASH_FA_EMBLEM_SIZE)
#define SPLASH_RAMG_C4 SPLASH_STAGE_NEXT(SPLASH_RAMG_C3, SPLASH_FA_WORDMARK_SIZE)
#define SPLASH_RAMG_C5 SPLASH_STAGE_NEXT(SPLASH_RAMG_C4, SPLASH_FA_BARS_CHROME_SIZE)
#define SPLASH_RAMG_C6 SPLASH_STAGE_NEXT(SPLASH_RAMG_C5, SPLASH_FA_LINE_BLUE_SIZE)
#define SPLASH_RAMG_CENTER_TOP SPLASH_STAGE_NEXT(SPLASH_RAMG_C6, SPLASH_FA_YEAR_BLUE_SIZE)

/* a side panel: background only */
#define SPLASH_RAMG_S1 SPLASH_STAGE_NEXT(SPLASH_RAMG_BASE, SPLASH_FA_METAL_TILE_SIZE)
#define SPLASH_RAMG_SIDE_TOP SPLASH_STAGE_NEXT(SPLASH_RAMG_S1, SPLASH_FA_GLOW_LEFT_SIZE)

static_assert(SPLASH_RAMG_CENTER_TOP <= (unsigned long)EVE_RAM_G_SIZE,
              "RAM_G overflow on the CENTRE panel: dash fonts plus the staged splash "
              "theme exceed 1 MiB. Assets staged past the ceiling are skipped at boot "
              "and the splash renders incomplete. Shrink the fonts or the assets -- see "
              "tools/make_material.py for how the background was cut from 614400 B to ~19 KB.");

static_assert(SPLASH_RAMG_SIDE_TOP <= (unsigned long)EVE_RAM_G_SIZE,
              "RAM_G overflow on a SIDE panel: dash fonts plus the staged background "
              "exceed 1 MiB.");

/* The glow slices are interchangeable per panel, so the centre-panel figure
 * above is only the true worst case while they are the same size. */
static_assert((SPLASH_FA_GLOW_CENTER_SIZE == SPLASH_FA_GLOW_LEFT_SIZE)
                  && (SPLASH_FA_GLOW_CENTER_SIZE == SPLASH_FA_GLOW_RIGHT_SIZE),
              "glow slices differ in size; the RAM_G budget above assumes they do not");

static const uint32_t CROSSFADE_MS = 400UL;

/* ---- RAM_G staging of the active theme (see header comment) ---- */

/* Per-asset RAM_G address once staged; 0 = not staged (draws skip the
 * asset for the session). Indexed like SPLASH_FLASH_ASSETS. */
static uint32_t g_splash_ramg[SPLASH_FA_COUNT];

/* Stage the active theme's assets (plus the theme-independent emblem and
 * wordmark) from the firmware-embedded pack straight into RAM_G above the
 * fonts, spot-checking the first 16 bytes of each readback against the
 * pack so a corrupted SPI write can never silently feed the renderer.
 * Returns false -- leaving the affected asset unstaged and skipped --
 * only when a readback verifiably lied or RAM_G would overflow. */
bool splash_stage_theme_to_ramg(const ThemeDesc *theme, uint8_t panel)
{
    const SplashFlashAsset *set[7];
    uint8_t n = 0U;
    /* Every panel carries the background so the cluster lights as one
     * surface; only the centre carries the artwork drawn on top of it. */
    set[n++] = theme->metal;
    set[n++] = theme->glow[panel < DASH_PANEL_COUNT ? panel : DASH_PANEL_CENTER];
    if (DASH_PANEL_CENTER == panel)
    {
        set[n++] = SPLASH_FA(SPLASH_FA_EMBLEM);
        set[n++] = SPLASH_FA(SPLASH_FA_WORDMARK);
        set[n++] = theme->side;
        set[n++] = theme->line;
        set[n++] = theme->year;
    }

    uint32_t addr = (g_ramg_fonts_end + 63UL) & ~63UL;
    bool all_ok = true;

    for (uint8_t i = 0U; i < n; i++)
    {
        const SplashFlashAsset *a = set[i];
        const uint32_t len = a->size;
        const uint32_t pack_off = a->addr - SPLASH_FLASH_BASE;
        if ((addr + len) > (uint32_t)EVE_RAM_G_SIZE)
        {
            /* continue, not break: breaking here silently took every asset
             * AFTER the oversized one with it. That is how the blue theme
             * lost both its accent line and the year for a month when only
             * the line actually overflowed. A later, smaller asset may still
             * fit, and one that does not says so on its own line. */
            Serial.printf("Splash staging OVERFLOW at %s (%lu B, %lu free) -> asset skipped\r\n",
                          a->name, (unsigned long)len,
                          (unsigned long)((uint32_t)EVE_RAM_G_SIZE - addr));
            all_ok = false;
            continue;
        }
        EVE_memWrite_flash_buffer(addr, &splash_flash_pack[pack_off], len);

        /* spot-check 16 bytes back over SPI (writes are trusted only after
         * a readback proves the link -- reads fail before writes on a
         * marginal bus, so this doubles as the clock-walk integrity gate) */
        bool match = true;
        for (uint32_t w = 0U; w < 16U; w += 4U)
        {
            uint32_t pack_word;
            memcpy(&pack_word, &splash_flash_pack[pack_off + w], 4UL);
            if (EVE_memRead32(addr + w) != pack_word)
            {
                match = false;
                break;
            }
        }
        if (!match)
        {
            Serial.printf("Splash staging MISCOMPARE at %s -> asset skipped\r\n", a->name);
            all_ok = false;
            continue; /* leave this asset unstaged; try the rest */
        }

        g_splash_ramg[(uint8_t)(a - SPLASH_FLASH_ASSETS)] = addr;
        addr = (addr + len + 63UL) & ~63UL;
    }

    Serial.printf("Splash staged to RAM_G panel %u: %u assets, top %lu (headroom %lu)\r\n",
                  (unsigned)panel, (unsigned)n, (unsigned long)addr,
                  (unsigned long)((uint32_t)EVE_RAM_G_SIZE - addr));
    return all_ok;
}

/* Bitmap source for a splash asset: its staged RAM_G address, or 0 when
 * staging failed -- the draw path skips unstaged assets (no fallback). */
static uint32_t splash_bitmap_source(const SplashFlashAsset *a)
{
    return g_splash_ramg[(uint8_t)(a - SPLASH_FLASH_ASSETS)];
}

/* Draw a staged ASTC bitmap 1:1 at (x, y) in pixels. CMD_SETBITMAP takes
 * extended (>17) formats and emits BITMAP_EXT_FORMAT plus the
 * LAYOUT(_H)/SIZE(_H) words itself (guide 5.75). Filter comes out NEAREST,
 * which is exact for 1:1 draws. An asset that failed staging draws nothing
 * -- it sits out the session rather than render from a source that lied. */
void draw_flash_asset(const SplashFlashAsset *a, int16_t x, int16_t y)
{
    const uint32_t src = splash_bitmap_source(a);
    if (0UL == src)
    {
        return;
    }
    EVE_cmd_setbitmap(src, (uint16_t)a->fmt, a->w, a->h);
    EVE_cmd_dl(DL_BEGIN | EVE_BITMAPS);
    EVE_cmd_dl(VERTEX2F((int16_t)(x * 16), (int16_t)(y * 16)));
    EVE_cmd_dl(DL_END);
}

/* How strongly the machining reads over the lit field. The tile is ADDED
 * (EVE has no multiply blend), so this scales the added highlight rather
 * than mixing two images -- which is also the physically sensible model: a
 * specular highlight adds light, it does not attenuate what is under it. */
#define SPLASH_METAL_STRENGTH 200U

/* Full-screen background in two layers.
 *
 *   glow  -- a 64x37 slice of one cluster-wide field, magnified 16x with
 *            BILINEAR. Authored small on purpose: a smooth gradient stored
 *            as a full-size ASTC bitmap comes back quantised into blocks,
 *            whereas a stretched small one is smooth by construction.
 *   metal -- a 128x128 seamless machined tile at NATIVE resolution, drawn
 *            with EVE_REPEAT so one tile covers the panel. Never scaled:
 *            magnifying it would destroy the machining it exists to show.
 *
 * BITMAP_SIZE carries only 9 bits of width/height, so a 1024x600 draw needs
 * BITMAP_SIZE_H alongside it for the high bits. */
void draw_splash_background(const ThemeDesc *theme, uint8_t panel, uint8_t alpha)
{
    const uint8_t idx = (panel < DASH_PANEL_COUNT) ? panel : DASH_PANEL_CENTER;
    const SplashFlashAsset *glow = theme->glow[idx];
    const SplashFlashAsset *metal = theme->metal;

    const uint32_t glow_src = splash_bitmap_source(glow);
    if (0UL != glow_src)
    {
        EVE_cmd_dl(COLOR_A(alpha));
        EVE_cmd_setbitmap(glow_src, (uint16_t)glow->fmt, glow->w, glow->h);
        EVE_cmd_dl(BITMAP_SIZE(EVE_BILINEAR, EVE_BORDER, EVE_BORDER,
                               (uint16_t)(EVE_HSIZE & 0x1FFU), (uint16_t)(EVE_VSIZE & 0x1FFU)));
        EVE_cmd_dl(BITMAP_SIZE_H((uint16_t)EVE_HSIZE, (uint16_t)EVE_VSIZE));
        EVE_cmd_dl(CMD_LOADIDENTITY);
        EVE_cmd_scale((long)(((long)EVE_HSIZE << 16) / (long)glow->w),
                      (long)(((long)EVE_VSIZE << 16) / (long)glow->h));
        EVE_cmd_dl(CMD_SETMATRIX);
        EVE_cmd_dl(DL_BEGIN | EVE_BITMAPS);
        EVE_cmd_dl(VERTEX2F(0, 0));
        EVE_cmd_dl(DL_END);
        EVE_cmd_dl(CMD_LOADIDENTITY);
        EVE_cmd_dl(CMD_SETMATRIX);
    }

    const uint32_t metal_src = splash_bitmap_source(metal);
    if (0UL != metal_src)
    {
        EVE_cmd_dl(COLOR_A((uint8_t)(((uint16_t)alpha * SPLASH_METAL_STRENGTH) / 255U)));
        EVE_cmd_dl(BLEND_FUNC(EVE_SRC_ALPHA, EVE_ONE)); /* additive: highlights ADD to the field */
        EVE_cmd_setbitmap(metal_src, (uint16_t)metal->fmt, metal->w, metal->h);
        EVE_cmd_dl(BITMAP_SIZE(EVE_NEAREST, EVE_REPEAT, EVE_REPEAT,
                               (uint16_t)(EVE_HSIZE & 0x1FFU), (uint16_t)(EVE_VSIZE & 0x1FFU)));
        EVE_cmd_dl(BITMAP_SIZE_H((uint16_t)EVE_HSIZE, (uint16_t)EVE_VSIZE));
        EVE_cmd_dl(DL_BEGIN | EVE_BITMAPS);
        EVE_cmd_dl(VERTEX2F(0, 0));
        EVE_cmd_dl(DL_END);
        EVE_cmd_dl(BLEND_FUNC(EVE_SRC_ALPHA, EVE_ONE_MINUS_SRC_ALPHA)); /* restore the default */
    }
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

    const uint32_t src = splash_bitmap_source(emblem);
    if (0UL == src)
    {
        return; /* failed staging: skip, same contract as draw_flash_asset --
                 * address 0 is the FONT data, never a drawable emblem */
    }
    EVE_cmd_dl(COLOR_A(alpha));
    EVE_cmd_setbitmap(src, (uint16_t)emblem->fmt,
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

    draw_splash_background(theme, DASH_PANEL_CENTER, global_alpha);

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
        const uint32_t t = min(now, (uint32_t) SPLASH_DURATION_MS); /* cast: STM32 std::min needs matching types */

        eve_frame_begin(0x000000UL);
        draw_splash_elements(theme, t, 255U);
        eve_frame_end();

        /* the sides carry the background for the whole animation, so the
         * cluster lights as one surface rather than one panel between two
         * black ones; they reselect the centre before returning */
        dash_sides_frame(0U, theme, 255U, 0x000000UL);

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
     * crossfade; splash assets and dash fonts are RAM_G co-tenants, both
     * staged before the splash began, so both stay drawable throughout) */
    const uint32_t f0 = millis();
    for (;;)
    {
        const uint32_t fnow = millis() - f0;
        const uint32_t ft = min(fnow, (uint32_t) CROSSFADE_MS); /* cast: STM32 std::min needs matching types */
        const uint8_t in_a = (uint8_t)((ft * 255UL) / CROSSFADE_MS);
        const uint8_t out_a = (uint8_t)(255U - in_a);

        eve_frame_begin(COLOR_BG);
        draw_splash_elements(theme, SPLASH_DURATION_MS, out_a);
        draw_dash_content(millis(), in_a);
        eve_frame_end();

        /* the sides fade in from black on this same alpha ramp, driven by
         * the one shared crossfade (R8/KTD9) -- never their own timers;
         * dash_sides_frame() reselects the center before returning */
        dash_sides_frame(in_a, theme, out_a, COLOR_BG);

        if (ft >= CROSSFADE_MS) { break; }
    }
}

#endif /* SPLASH_RENDER_H */
