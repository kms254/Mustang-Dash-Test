/*
 * splash_render.h - the boot splash, whole: theme/asset descriptors, the
 * one-time QSPI flash provisioning of the ASTC pack, the splash element
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

/* Splash assets are ASTC bitmaps STORED in the panel's 64 MB QSPI flash
 * (address table + provisioning pack in splash_flash.h) but RENDERED from
 * RAM_G: the active theme's assets are staged flash->RAM_G once at boot
 * (splash_stage_theme_to_ramg) and every draw sources RAM_G.
 *
 * Why not render direct from flash (the original design): bench forensics
 * 2026-07-10 -- long ASTC bursts garbled while the command path
 * (CMD_FLASHREAD/UPDATE, CRC) stayed fully healthy. Originally read as
 * damage to one module; 2026-07-21 established it is a per-frame bandwidth
 * ceiling every module has. The graphics engine cannot fetch a large asset
 * from flash within the frame budget, so scanlines drop -- content is
 * correct, the delivery rate is not. It persists with REG_FLASH_STATUS == 3
 * (full speed); CMD_FLASHFAST does not help. Bridgetek documents the limit
 * and puts the comfortable size at "a few tens of KBytes"; measured
 * threshold here is 40-56 KB per asset. See
 * docs/solutions/architecture-patterns/bt817-flash-render-streaming-bandwidth-ceiling.md
 *
 * Staging uses only the command path, and ASTC renders from RAM_G
 * flawlessly. Cost: one theme (<= 281 KB) resident above the fonts
 * (~285 KB), ~566 KB of the center's 1 MB RAM_G, with the flash-source path
 * kept as an automatic fallback if staging cannot run -- degraded, since
 * that path is the one subject to the ceiling. */

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

/* RAM_G scratch used only while provisioning the splash pack at boot:
 * a 64-byte header readback at the top of RAM_G and a 32 KB staging
 * buffer for CMD_FLASHUPDATE chunks just below it -- both far above the
 * fonts (~273 KB from address 0). */
static const uint32_t SCRATCH_HDR = 0xFF000UL;
static const uint32_t SCRATCH_BUF = 0xF7000UL;
static const uint32_t PROVISION_CHUNK = 0x8000UL; /* 32 KB, multiple of 4096 */

static const uint32_t CROSSFADE_MS = 400UL;

/* ---- RAM_G staging of the active theme (see header comment) ---- */

/* Per-asset RAM_G address once staged; 0 = not staged (draws fall back to
 * the direct flash source). Indexed like SPLASH_FLASH_ASSETS. */
static uint32_t g_splash_ramg[SPLASH_FA_COUNT];

/* Stage the active theme's assets (plus the theme-independent emblem and
 * wordmark) from flash into RAM_G above the fonts, spot-checking the first
 * 16 bytes of each against the embedded pack so a bad read can never
 * silently feed the renderer. Returns false -- and leaves the affected
 * asset on the flash-source fallback -- only when a read verifiably lied
 * or RAM_G would overflow. */
bool splash_stage_theme_to_ramg(const ThemeDesc *theme)
{
    const SplashFlashAsset *set[7];
    uint8_t n = 0U;
    set[n++] = SPLASH_FA(SPLASH_FA_EMBLEM);
    set[n++] = SPLASH_FA(SPLASH_FA_WORDMARK);
    set[n++] = theme->bg;
    set[n++] = theme->side;
    set[n++] = theme->line;
    set[n++] = theme->year;
    if (theme->has_strip)
    {
        set[n++] = theme->strip;
    }

    uint32_t addr = (g_ramg_fonts_end + 63UL) & ~63UL;
    bool all_ok = true;

    for (uint8_t i = 0U; i < n; i++)
    {
        const SplashFlashAsset *a = set[i];
        const uint32_t len = (a->size + 3UL) & ~3UL; /* FLASHREAD wants mult-of-4 */
        if ((addr + len) > (uint32_t)EVE_RAM_G_SIZE)
        {
            Serial.printf("Splash staging OVERFLOW at %s -> flash fallback\r\n", a->name);
            all_ok = false;
            break;
        }
        EVE_cmd_flashread(addr, a->addr, len);
        EVE_execute_cmd();

        /* spot-check 16 bytes against the embedded pack (command-path
         * reads are trusted only after they prove it) */
        bool match = true;
        const uint32_t pack_off = a->addr - SPLASH_FLASH_BASE;
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
            Serial.printf("Splash staging MISCOMPARE at %s -> flash fallback\r\n", a->name);
            all_ok = false;
            continue; /* leave this asset unstaged; try the rest */
        }

        g_splash_ramg[(uint8_t)(a - SPLASH_FLASH_ASSETS)] = addr;
        addr = (addr + len + 63UL) & ~63UL;
    }

    Serial.printf("Splash theme staged to RAM_G: %u assets, top %lu (headroom %lu)\r\n",
                  (unsigned)n, (unsigned long)addr,
                  (unsigned long)((uint32_t)EVE_RAM_G_SIZE - addr));
    return all_ok;
}

/* Bitmap source for a splash asset: its staged RAM_G copy when present,
 * else the direct flash source (bit 23 + 32-byte block address). */
static uint32_t splash_bitmap_source(const SplashFlashAsset *a)
{
    const uint32_t staged = g_splash_ramg[(uint8_t)(a - SPLASH_FLASH_ASSETS)];
    return (0UL != staged) ? staged : (0x800000UL | (a->addr / 32UL));
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
    EVE_cmd_setbitmap(splash_bitmap_source(a), (uint16_t)a->fmt, a->w, a->h);
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
    EVE_cmd_setbitmap(splash_bitmap_source(emblem), (uint16_t)emblem->fmt,
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

        /* the sides fade in from black on this same alpha ramp, driven by
         * the one shared crossfade (R8/KTD9) -- never their own timers;
         * dash_sides_frame() reselects the center before returning */
        dash_sides_frame(in_a);

        if (ft >= CROSSFADE_MS) { break; }
    }
}

#endif /* SPLASH_RENDER_H */
