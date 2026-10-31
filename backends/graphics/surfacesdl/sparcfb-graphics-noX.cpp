/* Allow getenv/etc — scummvm's common/forbidden.h gates these unless
 * the file opts out, like the SDL backend itself does. */
#define FORBIDDEN_SYMBOL_ALLOW_ALL

/*
 * sparcfb-graphics-noX.cpp — Native 8-bit graphics backend, X11-free.
 *
 * Subclasses SurfaceSdlGraphicsManager.  When a game engine starts
 * (initSize is called), switches to native 8-bit rendering via sparc_fb.
 * The launcher and overlay fall back to the parent SDL path.
 *
 * Pipeline collapses to ONE copy + scale per frame:
 *   engine writes INDEX8 into _native_pixels →
 *   scaleAndBlit() does pixel-replicate scale into _scaled_pixels →
 *   sparc_fb_ram_to_fb() writes _scaled_pixels to /dev/fb mmap.
 *
 * Palette changes go directly to the BT458 RAMDAC via sparc_fb_set_palette
 * — no pixel rerasterizing on palette cycling (the original X11 variant's
 * key win, here without X11 dependencies).
 *
 * Coexistence with SDL_sparcfb video driver: both mmap /dev/fb but only
 * one writes at a time. We set SDL_SPARCFB_NATIVE=1 hint so SDL_sparcfb
 * skips its own UpdateWindowFramebuffer blit. Input still flows through
 * SDL2 events (kbd/mouse via /dev/kbd in SDL_sparcfb).
 *
 * Part of the Sunstorm Project
 */

#include "backends/graphics/surfacesdl/sparcfb-dirty-copy.h"
#include "backends/graphics/surfacesdl/sparcfb-blit-plan.h"
#include "sparcfb-graphics-noX.h"
#include "common/textconsole.h"
#include "common/config-manager.h"
#include "engines/engine.h"
#include "common/file.h"
#include <unistd.h>
#include "common/debug.h"
#include "common/config-manager.h"
#include "sparc_fb.h"

#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>

/* Access the opaque sparc_fb context from the void pointer in the header */
#define _sparcfb_ctx ((sparc_fb_context *)_sparcfb_context_ptr)

/* ================================================================
 * Process-wide cleanup hooks
 *
 * The C++ destructor on SparcFbGraphicsManager isn't reliable for
 * tearing down the framebuffer state — scummvm's quit path can
 * call exit(3) (or even _exit(2)) without unwinding heap-allocated
 * OSystem internals, so the destructor never fires.  Instead we
 * register an atexit() handler in the constructor that does the
 * "clear FB + restore palette" step, plus chained signal handlers
 * for SIGTERM/SIGINT/SIGHUP/SIGQUIT mirroring sparc-input's
 * recovery shape.  SIGKILL still bypasses everything; that's what
 * a future fb_reset binary would cover.
 * ================================================================ */

static sparc_fb_context *g_cleanup_ctx = NULL;
static const int g_cleanup_signals[] = {
    SIGTERM, SIGINT, SIGHUP, SIGQUIT
};
#define G_CLEANUP_NSIGS (sizeof(g_cleanup_signals)/sizeof(g_cleanup_signals[0]))
static struct sigaction g_cleanup_prev[G_CLEANUP_NSIGS];
static int g_cleanup_prev_valid[G_CLEANUP_NSIGS];

static void sparcfb_do_cleanup(sparc_fb_context *ctx) {
    if (!ctx) return;
    /* Defensive null-checks on the live mappings — at atexit time the
     * graphics manager may already have munmap'd parts of the device
     * via its destructor (or SDL's VideoQuit ran before us in atexit
     * order), and writing through a stale fb_memory or stip_base
     * pointer bus-errors and aborts the rest of the cleanup chain
     * (incl. sparc-input's kbd restore — which is why a bus here
     * leaves the keyboard wedged after Ctrl-Q). */
    if (ctx->have_dac && ctx->dac_base) {
        unsigned char gray[256 * 3];
        for (int i = 0; i < 256; i++) {
            gray[i * 3 + 0] = (unsigned char)i;
            gray[i * 3 + 1] = (unsigned char)i;
            gray[i * 3 + 2] = (unsigned char)i;
        }
        sparc_fb_set_palette(ctx, 0, 256, gray);
    }
    /* sparc_fb_fill_rect dispatches to the STIP engine when have_stip
     * is set, which writes to stip_base — that's a separate mapping
     * sparc_fb_shutdown comments-out unmapping of, but it's still
     * worth guarding since fb_memory + dimensions need to be live too. */
    if (ctx->fb_memory && ctx->fb_width > 0 && ctx->fb_height > 0) {
        sparc_fb_fill_rect(ctx, 0, 0, ctx->fb_width, ctx->fb_height, 0);
    }
}

static void sparcfb_atexit_cleanup(void) {
    sparc_fb_context *ctx = g_cleanup_ctx;
    if (!ctx) return;
    g_cleanup_ctx = NULL; /* idempotent */
    sparcfb_do_cleanup(ctx);
}

static void sparcfb_signal_cleanup(int signo) {
    sparcfb_atexit_cleanup();
    /* Chain to prior handler so SDL/libc see the signal too. */
    unsigned int i;
    for (i = 0; i < G_CLEANUP_NSIGS; i++) {
        if (g_cleanup_signals[i] == signo && g_cleanup_prev_valid[i]) {
            const struct sigaction *prev = &g_cleanup_prev[i];
            if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction) {
                prev->sa_sigaction(signo, NULL, NULL);
                return;
            }
            if (prev->sa_handler == SIG_DFL) {
                signal(signo, SIG_DFL);
                raise(signo);
                return;
            }
            if (prev->sa_handler == SIG_IGN) {
                return;
            }
            if (prev->sa_handler) {
                prev->sa_handler(signo);
                return;
            }
        }
    }
    raise(signo);
}

static void sparcfb_install_cleanup(sparc_fb_context *ctx) {
    g_cleanup_ctx = ctx;
    atexit(sparcfb_atexit_cleanup);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sparcfb_signal_cleanup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;

    for (unsigned int i = 0; i < G_CLEANUP_NSIGS; i++) {
        struct sigaction old;
        if (sigaction(g_cleanup_signals[i], &sa, &old) == 0) {
            g_cleanup_prev[i] = old;
            g_cleanup_prev_valid[i] = 1;
        }
    }
}

/* ================================================================
 * Construction / destruction
 * ================================================================ */

SparcFbGraphicsManager::SparcFbGraphicsManager(SdlEventSource *sdlEventSource,
                                                 SdlWindow *window)
    : SurfaceSdlGraphicsManager(sdlEventSource, window),
      _sparcfb_context_ptr(NULL),
      _sparcfb_initialized(false),
      _launcher_width(0), _launcher_height(0),
      _native_pixels(NULL),
      _native_width(0), _native_height(0), _native_pitch(0),
      _scaled_pixels(NULL),
      _scaled_width(0), _scaled_height(0), _scaled_pitch(0),
      _scale_factor(3),
      _fb_offset_x(0), _fb_offset_y(0),
      _native_palette_dirty(false),
      _native_margins_dirty(true),
      _native_margin_color(0),
      _native_mode(false),
      _native_cursor_pixels(NULL),
      _native_cursor_w(0), _native_cursor_h(0),
      _native_cursor_hotx(0), _native_cursor_hoty(0),
      _native_cursor_keycolor(0),
      _native_cursor_visible(false),
      _native_mouse_x(0), _native_mouse_y(0),
      _last_native_pixels(NULL),
      _last_native_valid(false),
      _dirty_tiles(NULL), _tiles_x(0), _tiles_y(0),
      _dirty_disabled(false),
      _lockscreen_called(false),
      _last_cursor_drawn_x(0), _last_cursor_drawn_y(0),
      _last_cursor_drawn_w(0), _last_cursor_drawn_h(0),
      _was_cursor_visible(false),
      _lb_top_detected(0), _lb_bot_detected(0),
      _lb_top_applied(0), _lb_bot_applied(0),
      _lb_stable_frames(0), _current_win_h(0)
{
    // A framebuffer session is a launcher: engine quit returns here by
    // default, while an explicit user preference may still override it.
    ConfMan.registerDefault("gui_return_to_launcher_at_exit", true);

    memset(_native_palette, 0, sizeof(_native_palette));

    /* Read scale factor from env (matches sparcfb SDL2 driver convention) */
    const char *env_scale = getenv("SPARCFB_SCALE");
    if (env_scale) {
        int s = atoi(env_scale);
        if (s >= 1 && s <= 8) _scale_factor = s;
    }

    /* Initialize sparc_fb library */
    _sparcfb_context_ptr = (void *)new sparc_fb_context();
    memset(_sparcfb_context_ptr, 0, sizeof(sparc_fb_context));

    /* Don't preemptively claim /dev/fb here — defer the
     * SDL_SPARCFB_NATIVE=1 hint until initSize() actually confirms an
     * INDEX8 game (the only format our native pipeline handles).  If
     * we set it eagerly and then a 16-/32-bit engine like ultima8
     * starts up, _native_mode never goes true, our updateScreen falls
     * through to parent's SDL path, parent calls
     * SDL_UpdateWindowSurface, but SDL_sparcfb ignores the call
     * because of the hint we set — result is audio with no video. */

    if (sparc_fb_init(&(*_sparcfb_ctx)) == 0) {
        _sparcfb_initialized = true;
        _launcher_width = _sparcfb_ctx->fb_width;
        _launcher_height = _sparcfb_ctx->fb_height;

        /* Defensive: allow runtime override of fb_linebytes if the
         * kernel reports a stride that doesn't match the visible width.
         * sparc_fb_init defaults fb_linebytes = fb_width which is correct
         * for all observed TCX configurations on SS-4, but if a future
         * board pads the stride, set SPARCFB_LINEBYTES to override. */
        const char *env_stride = getenv("SPARCFB_LINEBYTES");
        if (env_stride) {
            int s = atoi(env_stride);
            if (s >= _sparcfb_ctx->fb_width && s <= 4096) {
                _sparcfb_ctx->fb_linebytes = s;
                warning("sparcfb-gfx-noX: fb_linebytes overridden via env to %d", s);
            }
        }

        warning("sparcfb-gfx-noX: sparc_fb_init OK, fb=%dx%d, depth=%d, linebytes=%d, fb_size=%d, have_dac=%d, scale=%d",
                _sparcfb_ctx->fb_width, _sparcfb_ctx->fb_height,
                _sparcfb_ctx->fb_depth, _sparcfb_ctx->fb_linebytes,
                _sparcfb_ctx->fb_size, _sparcfb_ctx->have_dac,
                _scale_factor);

        /* Register process-wide cleanup hooks (atexit + signals) so a
         * Ctrl-Q exit, normal main() return, or any catchable signal
         * leaves the FB cleared and the palette set to linear grayscale
         * — even when scummvm's quit path takes a shortcut around
         * C++ destructors. */
        sparcfb_install_cleanup(&(*_sparcfb_ctx));
    } else {
        warning("sparcfb-gfx-noX: sparc_fb_init failed, will use SDL fallback");
        SDL_SetHint("SDL_SPARCFB_NATIVE", "0");
    }
}

SparcFbGraphicsManager::~SparcFbGraphicsManager() {
    delete[] (_native_pixels);
    delete[] (_scaled_pixels);
    delete[] (_native_cursor_pixels);
    delete[] (_last_native_pixels);
    delete[] (_dirty_tiles);
    if (_sparcfb_initialized) {
        /* Run the visible-state cleanup here too — destructor and
         * atexit cover different exit paths (destructor: clean OSystem
         * tear-down; atexit: short-circuit exit() that skips the
         * tear-down).  NULL the global ctx pointer first so the
         * atexit handler becomes a no-op if both end up firing. */
        sparc_fb_context *ctx = &(*_sparcfb_ctx);
        if (g_cleanup_ctx == ctx) g_cleanup_ctx = NULL;
        sparcfb_do_cleanup(ctx);
        /* Restore THC windowing AFTER the cleanup paint so any rows
         * outside the active window get the cleared/grayscale state
         * once the window is removed. */
        if (sparc_fb_window_active(ctx)) {
            sparc_fb_restore_window(ctx);
        }
        sparc_fb_shutdown(ctx);
    }
    delete (sparc_fb_context *)_sparcfb_context_ptr;
}

/* ================================================================
 * Tile dirty bitmap helpers
 * ================================================================ */

void SparcFbGraphicsManager::markAllTilesDirty() {
    if (_dirty_tiles)
        memset(_dirty_tiles, 1, _tiles_x * _tiles_y);
}

void SparcFbGraphicsManager::markTilesDirtyRect(int x, int y, int w, int h) {
    if (!_dirty_tiles || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= _native_width || y >= _native_height) return;
    if (x + w > _native_width)  w = _native_width  - x;
    if (y + h > _native_height) h = _native_height - y;
    if (w <= 0 || h <= 0) return;

    int tx0 = x / TILE_W;
    int tx1 = (x + w - 1) / TILE_W;
    int ty0 = y / TILE_H;
    int ty1 = (y + h - 1) / TILE_H;
    if (tx1 >= _tiles_x) tx1 = _tiles_x - 1;
    if (ty1 >= _tiles_y) ty1 = _tiles_y - 1;
    int ty;
    for (ty = ty0; ty <= ty1; ty++) {
        unsigned char *row = _dirty_tiles + ty * _tiles_x;
        int tx;
        for (tx = tx0; tx <= tx1; tx++) {
            row[tx] = 1;
        }
    }
}

/* ================================================================
 * Palette — direct RAMDAC, no X11
 * ================================================================ */

void SparcFbGraphicsManager::pushNativePalette() {
    if (!_sparcfb_initialized) return;

    /* sparc_fb_set_palette takes a flat byte array (R,G,B,R,G,B,...) — same
     * format we already store in _native_palette. Direct write to the
     * BT458 RAMDAC: 256 entries × 3 bytes via memory-mapped DAC port.
     * No pixel work — display hardware applies the new lookup on the next
     * pixel scanout. */
    sparc_fb_set_palette(&(*_sparcfb_ctx), 0, 256, _native_palette);
    _native_palette_dirty = false;
    // Margin pixels share the game's palette; never reserve or overwrite a colour.
    unsigned int best = ~0U;
    byte black = 0;
    for (int i = 0; i < 256; ++i) {
        const unsigned int r = _native_palette[i * 3];
        const unsigned int g = _native_palette[i * 3 + 1];
        const unsigned int b = _native_palette[i * 3 + 2];
        const unsigned int distance = r * r + g * g + b * b;
        if (distance < best) { best = distance; black = i; }
    }
    if (black != _native_margin_color) {
        _native_margin_color = black;
        _native_margins_dirty = true;
    }
    repaintNativeMargins();
}

// Use the mapped INDEX8 surface for margins. Hardware fill apertures have
// separate addressing assumptions and are unnecessary for these rare clears.
static void fillNativeMargin(sparc_fb_context *ctx, int x, int y, int w, int h, byte color) {
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; ++row)
        memset(ctx->fb_memory + row * ctx->fb_linebytes + x, color, w);
}

void SparcFbGraphicsManager::repaintNativeMargins() {
    if (!_native_margins_dirty || !_native_mode || isOverlayVisible() ||
        !_sparcfb_initialized || !_scaled_pixels) return;
    sparc_fb_context *ctx = _sparcfb_ctx;
    if (_fb_offset_x < 0 || _fb_offset_y < 0 || _scaled_width <= 0 || _scaled_height <= 0 ||
        _fb_offset_x + _scaled_width > ctx->fb_width ||
        _fb_offset_y + _scaled_height > ctx->fb_height) return;
    // Four disjoint rectangles outside the image: leave every game pixel intact.
    fillNativeMargin(ctx, 0, 0, ctx->fb_width, _fb_offset_y, _native_margin_color);
    const int bottom = _fb_offset_y + _scaled_height;
    fillNativeMargin(ctx, 0, bottom, ctx->fb_width, ctx->fb_height - bottom, _native_margin_color);
    fillNativeMargin(ctx, 0, _fb_offset_y, _fb_offset_x, _scaled_height, _native_margin_color);
    const int right = _fb_offset_x + _scaled_width;
    fillNativeMargin(ctx, right, _fb_offset_y, ctx->fb_width - right, _scaled_height, _native_margin_color);
    debug(1, "Native margins repainted: color=%u image=%dx%d at %d,%d display=%dx%d stride=%d",
          _native_margin_color, _scaled_width, _scaled_height, _fb_offset_x, _fb_offset_y,
          ctx->fb_width, ctx->fb_height, ctx->fb_linebytes);
    _native_margins_dirty = false;
}

/* Sync our _native_palette from the parent's _currentPalette (SDL_Color
 * array, populated whenever SurfaceSdlGraphicsManager::setPalette() runs).
 * Returns true if any color actually changed. */
bool SparcFbGraphicsManager::syncPaletteFromParent() {
    bool changed = false;
    int i;
    for (i = 0; i < 256; i++) {
        byte r = _currentPalette[i].r;
        byte g = _currentPalette[i].g;
        byte b = _currentPalette[i].b;
        if (_native_palette[i*3+0] != r ||
            _native_palette[i*3+1] != g ||
            _native_palette[i*3+2] != b) {
            _native_palette[i*3+0] = r;
            _native_palette[i*3+1] = g;
            _native_palette[i*3+2] = b;
            changed = true;
        }
    }
    return changed;
}

void SparcFbGraphicsManager::setPalette(const byte *colors, uint start, uint num) {
    /* Optional diagnostics — opt-in via SPARCFB_DEBUG_PALETTE=1.  Default
     * silent so log volume stays manageable in steady-state. */
    static const bool sst_log_palette = (getenv("SPARCFB_DEBUG_PALETTE") != NULL);
    if (sst_log_palette) {
        static int sp_n = 0;
        if (sp_n < 5 || (sp_n % 50) == 0) {
            warning("setPalette #%d start=%u num=%u native=%d sum=%d",
                    sp_n, start, num, _native_mode,
                    (int)colors[0] + colors[1] + colors[2] + (num > 1 ? colors[3]+colors[4]+colors[5] : 0));
        }
        sp_n++;
    }
    /* Store locally and push to RAMDAC immediately */
    memcpy(_native_palette + start * 3, colors, num * 3);
    if (_native_mode && _sparcfb_initialized)
        pushNativePalette();

    /* Also update parent (for overlay/launcher rendering, AND so that
     * _currentPalette stays in sync as a safety source we read in
     * updateScreen.) */
    SurfaceSdlGraphicsManager::setPalette(colors, start, num);
}

void SparcFbGraphicsManager::grabPalette(byte *colors, uint start, uint num) const {
    memcpy(colors, _native_palette + start * 3, num * 3);
}

/* ================================================================
 * initSize — switch to native 8-bit mode
 * ================================================================ */

void SparcFbGraphicsManager::initSize(uint width, uint height,
                                        const Graphics::PixelFormat *format) {
    // ScummVM calls setupGraphics() before the launcher and again after an
    // engine is destroyed. Its nominal 320x200 size is not a game request.
    // Keep the launcher at the original desktop resolution, with SDL owning
    // the GUI surface; engine initSize calls below select the game mode.
    if (_sparcfb_initialized && getenv("SPARCFB_MODE_FD") && !g_engine) {
        SDL_SetHint("SDL_SPARCFB_LAUNCHER", "1");
        _native_mode = false;
        _native_palette_dirty = false;
        SDL_SetHint("SDL_SPARCFB_NATIVE", "0");
        SDL_SetHint("SDL_SPARCFB_GAME_WIDTH", Common::String::format("%d", _launcher_width).c_str());
        SDL_SetHint("SDL_SPARCFB_GAME_HEIGHT", Common::String::format("%d", _launcher_height).c_str());
        if (sparc_fb_set_mode(_sparcfb_ctx, _launcher_width, _launcher_height) < 0)
            error("Unable to restore supervised TCX launcher mode");
        SDL_SetHint("SDL_SPARCFB_DISPLAY_WIDTH", Common::String::format("%d", _launcher_width).c_str());
        SDL_SetHint("SDL_SPARCFB_DISPLAY_HEIGHT", Common::String::format("%d", _launcher_height).c_str());
        SurfaceSdlGraphicsManager::setScaler(_videoMode.scalerIndex, 1);
        const Graphics::PixelFormat guiFormat(2, 5, 6, 5, 0, 11, 5, 0, 0);
        SurfaceSdlGraphicsManager::initSize(_launcher_width, _launcher_height, &guiFormat);
        debug(1, "TCX launcher: %dx%d, SDL GUI presentation", _launcher_width, _launcher_height);
        return;
    }

    SDL_SetHint("SDL_SPARCFB_LAUNCHER", "0");

    /* All contexts share the guardian's mode. Request before SDL allocates
     * surfaces; update this direct INDEX8 context as well as SDL's context. */
    if (_sparcfb_initialized && getenv("SPARCFB_MODE_FD")) {
        const char *scaleEnv = getenv("SPARCFB_SCALE");
        _scale_factor = scaleEnv ? atoi(scaleEnv) : 1;
        if (_scale_factor < 1 || _scale_factor > 8) _scale_factor = 1;
        while (_scale_factor > 1 &&
               (width > 1152U / _scale_factor || height > 900U / _scale_factor))
            --_scale_factor;
        if (sparc_fb_set_mode(_sparcfb_ctx, width * _scale_factor, height * _scale_factor) < 0)
            error("Unable to select supervised TCX mode for %ux%u at %dx", width, height, _scale_factor);
        SDL_SetHint("SDL_SPARCFB_DISPLAY_WIDTH", Common::String::format("%d", _sparcfb_ctx->fb_width).c_str());
        SDL_SetHint("SDL_SPARCFB_DISPLAY_HEIGHT", Common::String::format("%d", _sparcfb_ctx->fb_height).c_str());
        SDL_SetHint("SDL_SPARCFB_GAME_WIDTH", Common::String::format("%u", width).c_str());
        SDL_SetHint("SDL_SPARCFB_GAME_HEIGHT", Common::String::format("%u", height).c_str());
    }

    /* Call parent first to set up SDL surfaces (needed for overlay/launcher) */
    SurfaceSdlGraphicsManager::initSize(width, height, format);

    if (!_sparcfb_initialized) return;

    /* Native pipeline only handles INDEX8 (8-bit paletted).  If the
     * engine asks for a higher-color format (RGB565, ARGB8888, etc.)
     * fall back to the SDL path so SDL_sparcfb's RGB→INDEX8 quantizer
     * draws the frame.  ultima8 is a notable case — runs in 16/32-bit
     * for its 3D-ish renderer.  Without this branch you get audio +
     * black screen because both pipelines stay quiet. */
    bool native_capable = (format == nullptr || format->bytesPerPixel == 1);
    if (!native_capable) {
        _native_mode = false;
        SDL_SetHint("SDL_SPARCFB_NATIVE", "0");
        warning("sparcfb-gfx-noX: engine requested %d bpp; deferring to SDL_sparcfb",
                format ? format->bytesPerPixel * 8 : 0);
        return;
    }

    /* INDEX8 game — claim /dev/fb. */
    SDL_SetHint("SDL_SPARCFB_NATIVE", "1");
    _native_mode = true;
    _native_width = width;
    _native_height = height;
    _native_pitch = (width + 7) & ~7;  /* 8-byte aligned for ldd/std */

    /* Allocate native shadow buffer + last-frame cache for frame-skip */
    delete[] (_native_pixels);
    _native_pixels = new byte[_native_pitch * _native_height]();
    delete[] (_last_native_pixels);
    _last_native_pixels = new byte[_native_pitch * _native_height]();
    _last_native_valid = false;

    /* Set up lockScreen surface (engines use lockScreen() for direct access) */
    _native_surface.init(_native_width, _native_height, _native_pitch,
                         _native_pixels, Graphics::PixelFormat::createFormatCLUT8());

    /* Calculate scaled dimensions */
    int fb_width = sparc_fb_fb_width(&(*_sparcfb_ctx));
    int fb_height = sparc_fb_fb_height(&(*_sparcfb_ctx));

    /* Auto-reduce scale if it exceeds FB */
    while (_scale_factor > 1 &&
           ((int)(width * _scale_factor) > fb_width ||
            (int)(height * _scale_factor) > fb_height))
        _scale_factor--;

    _scaled_width = _native_width * _scale_factor;
    _scaled_height = _native_height * _scale_factor;
    _scaled_pitch = (_scaled_width + 7) & ~7;

    /* THC vertical-only windowing — STOCK-ROM ALTERNATIVE to mode change.
     *
     * Tell the THC to scan only `_scaled_height` rows (instead of the full
     * fb_height). This reduces per-frame VRAM read bandwidth by the ratio
     * (_scaled_height / fb_height). For scale=3 that's 720/900 = 80% of
     * original — about 20% bandwidth save.
     *
     * Vertical-only because hde register doesn't actually constrain visible
     * width on real SS4 hardware (proven 2026-04-27). Horizontal stays full.
     *
     * After windowing, the visible region in VRAM is rows 0..(scaled_h-1),
     * full fb_width wide. So _fb_offset_y must be 0 — game pixels go at
     * the TOP of VRAM, not centered in fb_height. Horizontal centering
     * within full fb_width still applies via _fb_offset_x.
     *
     * Skip if SPARCFB_NO_WINDOW=1 is set (e.g. for debugging). */
    /* THC windowing disabled by default — keep the LCD on the full
     * 1152x900 active region and just center the rendered game in
     * VRAM via _fb_offset_y.  Avoids the LCD-side flakiness around
     * shrunken active regions and the asymmetric on-LCD position
     * that the windowed mode produced (the original frame's blanking
     * is heavier on top than bottom).  The set_window_centered
     * machinery + LCD-min floor stay in libsparc_fb for callers that
     * want them; just nothing in scummvm currently does.
     *
     * If a previous game session left a window applied, undo it so
     * the THC is back to full active video before we start. */
    if (sparc_fb_window_active(&(*_sparcfb_ctx))) {
        sparc_fb_restore_window(&(*_sparcfb_ctx));
    }

    /* Letterbox-detection state stays initialised to zero — it's
     * inert with the windowing path disabled but kept around so the
     * blit math compiles unchanged. */
    _lb_top_detected = _lb_bot_detected = 0;
    _lb_top_applied  = _lb_bot_applied  = 0;
    _lb_stable_frames = 0;
    _current_win_h   = 0;

    /* Center the rendered region in the full 1152x900 framebuffer —
     * THC drives all 900 active scanlines and the dark margins above
     * and below the game are just driven-zero pixels. */
    _fb_offset_x = (fb_width - _scaled_width) / 2;
    _fb_offset_y = (fb_height - _scaled_height) / 2;
    if (_fb_offset_x < 0) _fb_offset_x = 0;
    if (_fb_offset_y < 0) _fb_offset_y = 0;

    _native_margins_dirty = true;

    /* Allocate scaled output buffer */
    delete[] (_scaled_pixels);
    _scaled_pixels = new byte[_scaled_pitch * _scaled_height]();

    /* Allocate dirty-tile bitmap, mark every tile dirty so the first
     * frame paints in full.  Falls back to full-frame blit if width
     * isn't a TILE_W multiple — partial-edge tiles would yield
     * non-8-aligned ldd_copy widths and slow the MMIO path enough to
     * negate the dirty-rect win for that edge column. */
    delete[] (_dirty_tiles);
    _tiles_x = (_native_width  + TILE_W - 1) / TILE_W;
    _tiles_y = (_native_height + TILE_H - 1) / TILE_H;
    _dirty_tiles = new unsigned char[_tiles_x * _tiles_y]();
    markAllTilesDirty();
    {
        const char *e = getenv("SPARCFB_NO_DIRTY");
        _dirty_disabled = (e && e[0] == '1') ||
                          (_native_width % TILE_W != 0);
    }
    _was_cursor_visible = false;
    _last_cursor_drawn_x = _last_cursor_drawn_y = 0;
    _last_cursor_drawn_w = _last_cursor_drawn_h = 0;

    /* Seed RAMDAC with parent's current palette (it tracks scumm's
     * setPalette() pushes via parent::setPalette in our override).  If
     * scumm hasn't pushed yet, _currentPalette is all-zero and we'll
     * push that — it gets overwritten on the next setPalette call.
     * NB: we do NOT seed with grayscale; the grayscale init in earlier
     * iterations made the "stuck on grayscale" bug invisible by hiding
     * the all-black state behind a gradient. Better to see all-black
     * (palette unset) than grayscale (palette mis-set). */
    syncPaletteFromParent();
    /* Override entries 1-3 with bright RGB for the SPARCFB_TESTPAT path. */
    if (getenv("SPARCFB_TESTPAT")) {
        _native_palette[1*3+0] = 255; _native_palette[1*3+1] = 0;   _native_palette[1*3+2] = 0;
        _native_palette[2*3+0] = 0;   _native_palette[2*3+1] = 255; _native_palette[2*3+2] = 0;
        _native_palette[3*3+0] = 0;   _native_palette[3*3+1] = 0;   _native_palette[3*3+2] = 255;
    }
    pushNativePalette();

    /* SPARCFB_TESTPAT=1: skip game blit, draw a known full-FB pattern
     * directly to /dev/fb. Vertical stripes 16 px wide alternating
     * red/green/blue for stride/scale verification. */
    if (getenv("SPARCFB_TESTPAT")) {
        const int W = _sparcfb_ctx->fb_width;
        const int H = _sparcfb_ctx->fb_height;
        unsigned char *pat = new unsigned char[W * H];
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int stripe = (x / 16) % 3;
                pat[y * W + x] = stripe + 1;  /* uses palette idx 1/2/3 */
            }
        }
        sparc_fb_ram_to_fb(&(*_sparcfb_ctx), 0, 0, pat, W, W, H);
        delete[] pat;
        warning("sparcfb-gfx-noX: TESTPAT written, %dx%d vertical RGB stripes 16px wide", W, H);
        sleep(15);  /* hold pattern long enough to read off */
    }

    if (getenv("SPARCFB_MODE_FD"))
        fprintf(stderr, "TCX content: game=%ux%u scale=%d image=%dx%d display=%dx%d offset=%d,%d\n",
                width, height, _scale_factor, _scaled_width, _scaled_height,
                _sparcfb_ctx->fb_width, _sparcfb_ctx->fb_height, _fb_offset_x, _fb_offset_y);

    debug(1, "sparcfb-gfx-noX: initSize %ux%u, scaled to %dx%d at offset (%d,%d)",
          width, height, _scaled_width, _scaled_height, _fb_offset_x, _fb_offset_y);
}

/* ================================================================
 * Screen rendering — direct 8-bit, no RGB565
 * ================================================================ */

void SparcFbGraphicsManager::copyRectToScreen(const void *buf, int pitch,
                                                int x, int y, int w, int h) {
    if (!_native_mode || !_native_pixels) {
        SurfaceSdlGraphicsManager::copyRectToScreen(buf, pitch, x, y, w, h);
        return;
    }

    /* Optional rect-call logging — opt-in via SPARCFB_DEBUG_RECTS=1. */
    static const bool sst_log_rects = (getenv("SPARCFB_DEBUG_RECTS") != NULL);
    if (sst_log_rects) {
        static int crts_n = 0;
        if (crts_n < 3) {
            warning("crts #%d: x=%d y=%d w=%d h=%d pitch=%d native=%dx%d npitch=%d",
                    crts_n, x, y, w, h, pitch,
                    _native_width, _native_height, _native_pitch);
        }
        crts_n++;
    }

    const byte *src = (const byte *)buf;
    // AGS submits a whole screen even for a small GUI change. Do not
    // promote that submission into a full-frame VRAM update automatically.
    if (!_dirty_disabled && _dirty_tiles && x >= 0 && y >= 0 &&
        x + w <= _native_width && y + h <= _native_height) {
        if (!SparcFbDirty::compare(src, pitch, _native_pixels, _native_pitch,
                                  x, y, w, h, _dirty_tiles, _tiles_x, TILE_W, TILE_H))
            return;
    } else {
        markTilesDirtyRect(x, y, w, h);
    }
    byte *dest = _native_pixels + y * _native_pitch + x;
    for (int row = 0; row < h; ++row) {
        memcpy(dest, src, w);
        src += pitch;
        dest += _native_pitch;
    }
}

Graphics::Surface *SparcFbGraphicsManager::lockScreen() {
    /* Optional first-call lockScreen logging — opt-in via SPARCFB_DEBUG_RECTS=1. */
    static const bool sst_log_locks = (getenv("SPARCFB_DEBUG_RECTS") != NULL);
    if (sst_log_locks) {
        static int ls_n = 0;
        if (ls_n < 5) {
            warning("lockScreen #%d native_mode=%d native_pixels=%p surf=%dx%d pitch=%d fmt.bpp=%d",
                    ls_n, _native_mode, _native_pixels,
                    _native_surface.w, _native_surface.h,
                    (int)_native_surface.pitch,
                    (int)_native_surface.format.bytesPerPixel * 8);
        }
        ls_n++;
    }
    if (_native_mode && _native_pixels) {
        /* Engine is about to write directly into _native_pixels via the
         * returned Graphics::Surface — we can't see those writes through
         * markTilesDirtyRect.  Set a flag so scaleAndBlit() does a
         * per-tile diff against the prior-frame snapshot (saved in
         * _last_native_pixels) and marks only changed tiles dirty.
         *
         * This matters for FM-TOWNS engines (Indy3 etc.) which lock
         * the system surface every frame to composite layers — without
         * the diff, every frame would be 100% dirty and dirty-rect
         * savings would be zero. */
        _lockscreen_called = true;
        return &_native_surface;
    }
    return SurfaceSdlGraphicsManager::lockScreen();
}

void SparcFbGraphicsManager::unlockScreen() {
    if (_native_mode)
        return;  /* native surface doesn't need unlock */
    SurfaceSdlGraphicsManager::unlockScreen();
}

void SparcFbGraphicsManager::fillScreen(uint32 col) {
    if (_native_mode && _native_pixels) {
        memset(_native_pixels, col, _native_pitch * _native_height);
        markAllTilesDirty();
        return;
    }
    SurfaceSdlGraphicsManager::fillScreen(col);
}

void SparcFbGraphicsManager::fillScreen(const Common::Rect &r, uint32 col) {
    if (_native_mode && _native_pixels) {
        int row;
        for (row = r.top; row < r.bottom && row < _native_height; row++)
            memset(_native_pixels + row * _native_pitch + r.left, col, r.width());
        markTilesDirtyRect(r.left, r.top, r.width(), r.height());
        return;
    }
    SurfaceSdlGraphicsManager::fillScreen(r, col);
}

/* ================================================================
 * updateScreen — the fast path
 * ================================================================ */

// Optional on-demand Solaris CPU sampling. Ordinary play leaves it disabled.
// Creating <output>.start requests one 20-second capture without restarting.
class SSTNativeCpuProfile {
    bool _configured, _enabled, _running;
    uint32 _lastCheck, _started, _offset;
    unsigned short *_samples;
    Common::String _output;
    static const uint32 kScale = 4096;
    static const uint32 kBins = (0x01800000u / 16u) / 2u;
public:
    SSTNativeCpuProfile() : _configured(false), _enabled(false), _running(false),
        _lastCheck(0), _started(0), _offset(0), _samples(nullptr) {}
    ~SSTNativeCpuProfile() {
        if (_running) ::profil(nullptr, 0, 0, 0);
        delete[] _samples;
    }
    void tick() {
        if (!_configured) {
            _configured = true;
            if (ConfMan.hasKey("sst_native_profile_output"))
                _output = ConfMan.getPath("sst_native_profile_output").toString(Common::Path::kNativeSeparator);
            _enabled = !_output.empty();
        }
        if (!_enabled) return;
        const uint32 now = g_system->getMillis();
        if (!_running) {
            if (now - _lastCheck < 1000) return;
            _lastCheck = now;
            const Common::String trigger = _output + ".start";
            FILE *flag = fopen(trigger.c_str(), "rb");
            if (!flag) return;
            // An optional hexadecimal base selects a shared-library address range.
            // Empty trigger files keep the executable-only capture unchanged.
            unsigned long requestedOffset = 0;
            const bool haveOffset = fscanf(flag, "%lx", &requestedOffset) == 1;
            fclose(flag);
            _offset = haveOffset && requestedOffset <= 0xfe800000UL
                ? static_cast<uint32>(requestedOffset) & ~0xfffu : 0;
            remove(trigger.c_str());
            _samples = new unsigned short[kBins]();
            ::profil(_samples, kBins * sizeof(unsigned short), _offset, kScale);
            _started = g_system->getMillis();
            _running = true;
            warning("SST native CPU profile started");
            return;
        }
        if (now - _started < 20000) return;
        ::profil(nullptr, 0, 0, 0);
        _running = false;
        uint32 count = 0;
        for (uint32 i = 0; i < kBins; ++i) count += _samples[i];
        const Common::String temp = _output + ".part";
        Common::DumpFile file;
        if (file.open(Common::Path(temp))) {
            file.write("SSTPROF1", 8);
            file.writeUint32LE(_offset);
            file.writeUint32LE(kScale);
            file.writeUint32LE(kBins);
            file.writeUint32LE(now - _started);
            // Solaris SPARC counters are big-endian uint16 values.
            file.write(_samples, kBins * sizeof(unsigned short));
            const bool failed = file.err();
            file.close();
            if (!failed && rename(temp.c_str(), _output.c_str()) == 0)
                warning("SST native CPU profile saved: %u samples in %u ms", count, now - _started);
            else warning("SST native CPU profile could not be finalized");
        } else warning("SST native CPU profile could not open output");
        delete[] _samples;
        _samples = nullptr;
        _lastCheck = now;
    }
};

void SparcFbGraphicsManager::updateScreen() {
    static SSTNativeCpuProfile cpuProfile;
    cpuProfile.tick();
    /* When the GUI overlay is up (launcher / save dialog), route the
     * frame through the parent SDL path.  Parent composites the
     * overlay over the game surface and calls SDL_UpdateWindowSurface,
     * which lands in SDL_sparcfb's UpdateWindowFramebuffer; the
     * showOverlay() override below clears SDL_SPARCFB_NATIVE so that
     * function actually writes to /dev/fb instead of returning early.
     *
     * Without this branch we'd run our native scaleAndBlit pipeline
     * (which doesn't know about the overlay surface), and the
     * overlay would render off-screen — exactly the SQ6 save-dialog
     * bug. */
    if (isOverlayVisible()) {
        SurfaceSdlGraphicsManager::updateScreen();
        return;
    }

    if (!_native_mode || !_sparcfb_initialized) {
        /* Fall back to SDL path for launcher/overlay */
        SurfaceSdlGraphicsManager::updateScreen();
        return;
    }

    /* setPalette() already pushed to RAMDAC; just clear parent's dirty
     * range so future state stays clean.  The _native_palette_dirty
     * flag covers the (currently unused) path where we set palette
     * entries internally without going through setPalette. */
    if (_paletteDirtyEnd != 0) {
        _paletteDirtyStart = 0;
        _paletteDirtyEnd = 0;
    }
    if (_native_palette_dirty) {
        pushNativePalette();
    }

    repaintNativeMargins();

    /* Scale + blit */
    scaleAndBlit();
}

void SparcFbGraphicsManager::showOverlay(bool inGUI) {
    const bool nativeBackground=_native_mode && _native_pixels && _screen &&
        _screenFormat.bytesPerPixel==1;
    if(nativeBackground) {
        // Native rendering bypasses the parent's screen and palette uploads.
        // Supply the real game image before the GUI snapshots its backdrop.
        SurfaceSdlGraphicsManager::copyRectToScreen(_native_pixels,_native_pitch,
            0,0,_native_width,_native_height);
        if(_screen->format->palette)
            SDL_SetPaletteColors(_screen->format->palette,_currentPalette,0,256);
    }
    SDL_SetHint("SDL_SPARCFB_NATIVE_OVERLAY",nativeBackground?"1":"0");
    SDL_SetHint("SDL_SPARCFB_NATIVE_OVERLAY_FULL","1");
    /* Hand /dev/fb back to SDL_sparcfb for the duration of the overlay
     * so its UpdateWindowFramebuffer actually paints (parent's
     * updateScreen is what composites the overlay surface, and it
     * relies on that path landing on the framebuffer). */
    SDL_SetHint("SDL_SPARCFB_NATIVE", "0");
    SurfaceSdlGraphicsManager::showOverlay(inGUI);
}

void SparcFbGraphicsManager::hideOverlay() {
    SurfaceSdlGraphicsManager::hideOverlay();
    SDL_SetHint("SDL_SPARCFB_NATIVE_OVERLAY","0");
    /* Restore native ownership.  The native palette is still valid in
     * our shadow (_native_palette), but the BT458 may have been
     * reprogrammed by SDL_sparcfb's own LUT path during the overlay —
     * mark dirty so the next updateScreen re-pushes it. */
    SDL_SetHint("SDL_SPARCFB_NATIVE", _native_mode ? "1" : "0");
    _native_palette_dirty = _native_mode;
    _native_margins_dirty = true;
    /* The dialog painted across whatever fb area SDL_sparcfb covered;
     * our dirty-tile optimization only redraws tiles whose game-side
     * pixels changed, so overlay leftovers persist on screen until the
     * tiles under them happen to repaint.  Force a full repaint on the
     * first post-overlay frame so the leftovers are wiped. */
    markAllTilesDirty();
}

void SparcFbGraphicsManager::internUpdateScreen() {
    /* When the overlay is visible, updateScreen() routes to
     * SurfaceSdlGraphicsManager::updateScreen() which calls back into
     * internUpdateScreen via the virtual dispatch — if we then re-call
     * updateScreen() here, we recurse infinitely until the stack
     * overflows.  Defer to parent's internUpdateScreen in overlay mode
     * so the SDL path completes the frame; otherwise run the native
     * pipeline (which handles palette push + scaleAndBlit). */
    if (_native_mode && !isOverlayVisible()) {
        updateScreen();
        return;
    }
    SurfaceSdlGraphicsManager::internUpdateScreen();
}

/* (Letterbox detection removed 2026-04-30 — was causing spurious
 * re-windows on Indy3 splash screens that shifted game content out of
 * view once the gameplay engine started.  The public symbol
 * `sparc_fb_detect_letterbox` and the SPARC_FB_MIN_WIN_HEIGHT floor
 * stay; per-frame dynamic shrinking does not.  The class state for
 * detection (_lb_*) stays initialized to 0 in the ctor, so the blit
 * loop's `src_row < _lb_top_applied` check is always false and the
 * code path collapses to the pre-detection behaviour.) */

void SparcFbGraphicsManager::scaleAndBlit() {
    if (!_native_pixels || !_scaled_pixels) return;


    /* DIAGNOSTIC: dump buffers every 100 frames if SPARCFB_DUMP=1.
     * Off by default — file I/O on Solaris 7 over /tmp is non-trivial cost. */
    static const bool sst_dump_on = (getenv("SPARCFB_DUMP") != NULL);
    if (sst_dump_on) {
        static int dump_n = 0;
        if (dump_n == 0 || (dump_n % 100) == 1) {
            FILE *f = fopen("/tmp/native_pixels.raw", "wb");
            if (f) {
                fwrite(_native_pixels, 1, _native_pitch * _native_height, f);
                fclose(f);
                warning("native_pixels.raw written: %dx%d pitch=%d size=%d",
                        _native_width, _native_height, _native_pitch,
                        _native_pitch * _native_height);
            }
        }
        dump_n++;
    }

    int scale = _scale_factor;
    /* When the cursor is invisible we can write directly to /dev/fb,
     * skipping the _scaled_pixels intermediate buffer entirely.  This
     * cuts ~691KB of per-frame RAM writes at scale=3.  When the cursor
     * is visible we still go via _scaled_pixels so compositeNativeCursor
     * has somewhere to draw. */
    bool direct_blit = !_native_cursor_visible;

    /* Audio-thread yield cadence — sched_yield() between row strips so
     * the SDL audio LWP doesn't starve through the 50-100 ms scale=3
     * blit.  Cheap when no other thread is ready.  Every 8 rows
     * (~30 yields/frame at scale=3) balances latency vs syscall cost.
     * Disable with SPARCFB_NO_YIELD=1. */
    static const bool sst_yield_off = (getenv("SPARCFB_NO_YIELD") != NULL);
    const bool batchedBlits = ConfMan.hasKey("sst_fb_batched_blits") && ConfMan.getBool("sst_fb_batched_blits");
    const bool packedScale = ConfMan.hasKey("sst_fb_packed_scale") && ConfMan.getBool("sst_fb_packed_scale");

    /* Frame-skip (opt-in via SPARCFB_FRAME_SKIP=1): if _native_pixels is
     * bit-identical to the prior frame AND cursor invisible, return early.
     * NOTE: on MicroSPARC-II SS-4 the memcmp+memcpy of 76 KB thrashes the
     * 8 KB L1 cache and ends up costing more than the blit itself for
     * animated workloads.  Off by default; enable when running mostly
     * static gameplay. */
    static const bool sst_frame_skip = (getenv("SPARCFB_FRAME_SKIP") != NULL);
    if (sst_frame_skip && _last_native_valid && !_native_cursor_visible) {
        if (memcmp(_native_pixels, _last_native_pixels,
                   _native_pitch * _native_height) == 0) {
            return;
        }
        memcpy(_last_native_pixels, _native_pixels, _native_pitch * _native_height);
    } else if (sst_frame_skip) {
        memcpy(_last_native_pixels, _native_pixels, _native_pitch * _native_height);
        _last_native_valid = true;
    }

    /* FPS counter */
    static int frame_count = 0;
    static struct timeval last_report;
    struct timeval frame_start, frame_end;

    gettimeofday(&frame_start, NULL);
    if (++frame_count == 1)
        gettimeofday(&last_report, NULL);

    /* ── Cursor bbox bookkeeping for dirty-rect blit ──
     * Mark the cursor's previous draw position dirty so the area gets
     * cleared (overwritten with game pixels) this frame, AND mark the
     * new position dirty so the cursor sprite paints into clean area.
     * Without this, a cursor that moves over static pixels would leave
     * a trail (old position never gets re-blit) and the new position
     * would be missing the cursor (current pixels haven't changed). */
    if (_native_cursor_visible) {
        if (_was_cursor_visible) {
            markTilesDirtyRect(_last_cursor_drawn_x, _last_cursor_drawn_y,
                               _last_cursor_drawn_w, _last_cursor_drawn_h);
        }
        int cx = _native_mouse_x - _native_cursor_hotx;
        int cy = _native_mouse_y - _native_cursor_hoty;
        markTilesDirtyRect(cx, cy, _native_cursor_w, _native_cursor_h);
        _last_cursor_drawn_x = cx;
        _last_cursor_drawn_y = cy;
        _last_cursor_drawn_w = _native_cursor_w;
        _last_cursor_drawn_h = _native_cursor_h;
    } else if (_was_cursor_visible) {
        markTilesDirtyRect(_last_cursor_drawn_x, _last_cursor_drawn_y,
                           _last_cursor_drawn_w, _last_cursor_drawn_h);
    }
    _was_cursor_visible = _native_cursor_visible;

    /* If lockScreen() was called this frame, we don't know which pixels
     * the engine touched through the returned surface.  Per-tile memcmp
     * against the prior-frame snapshot finds the changes.  Each tile is
     * 256 bytes (16×16) which fits comfortably in L1, and we early-exit
     * each tile on the first differing row, so most clean tiles cost
     * one cache line.  Skip tiles already marked dirty by other paths
     * (copyRectToScreen, fillScreen, cursor bbox) — they're going to
     * be blit anyway. */
    if (_lockscreen_called && _last_native_pixels && _last_native_valid) {
        int ty;
        for (ty = 0; ty < _tiles_y; ty++) {
            int row_start = ty * TILE_H;
            int row_end = row_start + TILE_H;
            if (row_end > _native_height) row_end = _native_height;
            unsigned char *trow = _dirty_tiles + ty * _tiles_x;
            int tx;
            for (tx = 0; tx < _tiles_x; tx++) {
                if (trow[tx]) continue;
                int x0 = tx * TILE_W;
                int x1 = x0 + TILE_W;
                if (x1 > _native_width) x1 = _native_width;
                int seg_w = x1 - x0;
                int row;
                for (row = row_start; row < row_end; row++) {
                    if (memcmp(_native_pixels      + row * _native_pitch + x0,
                               _last_native_pixels + row * _native_pitch + x0,
                               seg_w) != 0) {
                        trow[tx] = 1;
                        break;
                    }
                }
            }
        }
    } else if (_lockscreen_called && _last_native_pixels) {
        /* No prior snapshot — mark all dirty so the next frame has a
         * baseline to diff against. */
        markAllTilesDirty();
    }
    _lockscreen_called = false;

    /* SPARCFB_NO_DIRTY=1 forces every frame to be all-dirty — used for
     * A/B comparison vs the dirty-rect path.  initSize() also forces
     * all-dirty when _native_width % TILE_W != 0 (partial-edge tiles
     * would yield non-8-aligned ldd_copy widths). */
    if (_dirty_disabled) markAllTilesDirty();

    unsigned char *fb_base = (unsigned char *)_sparcfb_ctx->fb_memory
                             + _fb_offset_y * _sparcfb_ctx->fb_linebytes
                             + _fb_offset_x;
    int fb_stride = _sparcfb_ctx->fb_linebytes;

    /* Optional per-frame dirty-tile counter for the FPS log line. */
    int dirty_tile_count = 0;
    static const bool sst_debug_dirty = (getenv("SPARCFB_DEBUG_DIRTY") != NULL);
    if (sst_debug_dirty) {
        int total = _tiles_x * _tiles_y;
        int i;
        for (i = 0; i < total; i++) if (_dirty_tiles[i]) dirty_tile_count++;
    }

    if (direct_blit) {
        /* Direct-to-FB per-tile-row scale + blit.  Per src row in a
         * dirty tile_row, build only dirty tile spans into staged_row
         * and ldd_copy each span 'scale' times.  Clean tile_rows are
         * skipped entirely — both the CPU scale work and the MMIO
         * writes vanish, which is the whole point of dirty rects. */
        static byte staged_row[2048] __attribute__((aligned(8)));
        int ty;
        for (ty = 0; ty < _tiles_y; ty++) {
            unsigned char *trow = _dirty_tiles + ty * _tiles_x;
            int tx_first = -1, tx_scan;
            for (tx_scan = 0; tx_scan < _tiles_x; tx_scan++) {
                if (trow[tx_scan]) { tx_first = tx_scan; break; }
            }
            if (tx_first < 0) continue;

            int row_start = ty * TILE_H;
            int row_end = row_start + TILE_H;
            if (row_end > _native_height) row_end = _native_height;

            int src_row;
            for (src_row = row_start; src_row < row_end; src_row++) {
                /* Skip rows that fall in the stripped letterbox — those
                 * rows aren't supposed to land in the windowed VRAM area
                 * at all.  When letterbox detection isn't active,
                 * _lb_top_applied / _lb_bot_applied are 0 and this is
                 * a no-op. */
                if (src_row <  _lb_top_applied) continue;
                if (src_row >= _native_height - _lb_bot_applied) continue;
                int dst_native_row = src_row - _lb_top_applied;
                const byte *src_line = _native_pixels + src_row * _native_pitch;
                unsigned char *fb_row0 = fb_base + (dst_native_row * scale) * fb_stride;
                if (batchedBlits) {
                    SparcFbBlit::directRow(trow,tx_first,_tiles_x,TILE_W,_native_width,
                        scale,src_line,staged_row,packedScale,
                        [&](int x, const unsigned char *source, int width) {
                            if ((fb_stride&7)==0) {
                                sparc_fb_ldd_copy(fb_row0+x,fb_stride,source,0,width,scale);
                            } else {
                                // The driver's multi-row ldd path assumes aligned strides.
                                for (int sy=0;sy<scale;++sy)
                                    sparc_fb_ldd_copy(fb_row0+x+sy*fb_stride,fb_stride,source,0,width,1);
                            }
                        });
                } else {
                int tx;
                for (tx = tx_first; tx < _tiles_x; tx++) {
                    if (!trow[tx]) continue;
                    int x0 = tx * TILE_W;
                    int x1 = x0 + TILE_W;
                    if (x1 > _native_width) x1 = _native_width;
                    int seg_src_w = x1 - x0;
                    int seg_dst_off = x0 * scale;
                    int seg_dst_w = seg_src_w * scale;

                    if (scale == 3) {
                        if (packedScale) SparcFbScale::triple(src_line+x0,staged_row+seg_dst_off,x1-x0);
                        else {
                        int q;
                        for (q = x0; q < x1; q++) {
                            byte p = src_line[q];
                            int o = q * 3;
                            staged_row[o]     = p;
                            staged_row[o + 1] = p;
                            staged_row[o + 2] = p;
                        }
                        }
                        sparc_fb_ldd_copy(fb_row0 + seg_dst_off,                 fb_stride, staged_row + seg_dst_off, 0, seg_dst_w, 1);
                        sparc_fb_ldd_copy(fb_row0 + seg_dst_off + fb_stride,     fb_stride, staged_row + seg_dst_off, 0, seg_dst_w, 1);
                        sparc_fb_ldd_copy(fb_row0 + seg_dst_off + 2 * fb_stride, fb_stride, staged_row + seg_dst_off, 0, seg_dst_w, 1);
                    } else if (scale == 2) {
                        int q;
                        for (q = x0; q < x1; q++) {
                            byte p = src_line[q];
                            int o = q * 2;
                            staged_row[o]     = p;
                            staged_row[o + 1] = p;
                        }
                        sparc_fb_ldd_copy(fb_row0 + seg_dst_off,             fb_stride, staged_row + seg_dst_off, 0, seg_dst_w, 1);
                        sparc_fb_ldd_copy(fb_row0 + seg_dst_off + fb_stride, fb_stride, staged_row + seg_dst_off, 0, seg_dst_w, 1);
                    } else if (scale == 1) {
                        sparc_fb_ldd_copy(fb_row0 + seg_dst_off, fb_stride,
                                          src_line + x0, 0, seg_dst_w, 1);
                    } else {
                        int q;
                        for (q = x0; q < x1; q++) {
                            byte p = src_line[q];
                            int sx;
                            for (sx = 0; sx < scale; sx++)
                                staged_row[q * scale + sx] = p;
                        }
                        int sy;
                        for (sy = 0; sy < scale; sy++) {
                            sparc_fb_ldd_copy(fb_row0 + seg_dst_off + sy * fb_stride,
                                              fb_stride, staged_row + seg_dst_off,
                                              0, seg_dst_w, 1);
                        }
                    }
                }
                }
                if (!sst_yield_off && (src_row & 7) == 0) sched_yield();
            }
        }
    } else {
        /* Cursor-visible per-tile path:
         *   pass 1 — scale dirty src tiles into _scaled_pixels (RAM only)
         *   pass 2 — composite cursor onto _scaled_pixels (cursor bbox is
         *            already in dirty set, so its tiles got scaled in p1)
         *   pass 3 — ldd_copy dirty tile spans from _scaled_pixels to fb
         *
         * Two-pass scale-then-blit (instead of fused) is required because
         * compositeNativeCursor() needs the scaled game pixels in place
         * before painting the cursor on top. */
        int ty;
        for (ty = 0; ty < _tiles_y; ty++) {
            unsigned char *trow = _dirty_tiles + ty * _tiles_x;
            int tx_first = -1, tx_scan;
            for (tx_scan = 0; tx_scan < _tiles_x; tx_scan++) {
                if (trow[tx_scan]) { tx_first = tx_scan; break; }
            }
            if (tx_first < 0) continue;

            int row_start = ty * TILE_H;
            int row_end = row_start + TILE_H;
            if (row_end > _native_height) row_end = _native_height;

            int src_row;
            for (src_row = row_start; src_row < row_end; src_row++) {
                /* Same letterbox skip+remap as the direct_blit branch. */
                if (src_row <  _lb_top_applied) continue;
                if (src_row >= _native_height - _lb_bot_applied) continue;
                int dst_native_row = src_row - _lb_top_applied;
                const byte *src_line = _native_pixels + src_row * _native_pitch;
                byte *dst_first = _scaled_pixels + (dst_native_row * scale) * _scaled_pitch;
                int tx;
                for (tx = tx_first; tx < _tiles_x; tx++) {
                    if (!trow[tx]) continue;
                    int x0 = tx * TILE_W;
                    int x1 = x0 + TILE_W;
                    if (x1 > _native_width) x1 = _native_width;
                    int seg_dst_off = x0 * scale;
                    int seg_dst_w = (x1 - x0) * scale;

                    if (scale == 3) {
                        if (packedScale) SparcFbScale::triple(src_line+x0,dst_first+seg_dst_off,x1-x0);
                        else {
                        int q;
                        for (q = x0; q < x1; q++) {
                            byte p = src_line[q];
                            int o = q * 3;
                            dst_first[o]     = p;
                            dst_first[o + 1] = p;
                            dst_first[o + 2] = p;
                        }
                        }
                    } else if (scale == 2) {
                        int q;
                        for (q = x0; q < x1; q++) {
                            byte p = src_line[q];
                            int o = q * 2;
                            dst_first[o]     = p;
                            dst_first[o + 1] = p;
                        }
                    } else if (scale == 1) {
                        memcpy(dst_first + x0, src_line + x0, x1 - x0);
                    } else {
                        int q;
                        for (q = x0; q < x1; q++) {
                            byte p = src_line[q];
                            int sx;
                            for (sx = 0; sx < scale; sx++)
                                dst_first[q * scale + sx] = p;
                        }
                    }

                    int dup_row;
                    for (dup_row = 1; dup_row < scale; dup_row++) {
                        memcpy(_scaled_pixels + (dst_native_row * scale + dup_row) * _scaled_pitch + seg_dst_off,
                               dst_first + seg_dst_off, seg_dst_w);
                    }
                }
            }
        }

        compositeNativeCursor();

        /* Pass 3 — blit dirty tile spans from _scaled_pixels to fb,
         * one fb row at a time so each ldd_copy span starts at the
         * tile's 8-byte-aligned destination offset.
         *
         * After letterbox stripping, Pass 1 wrote _scaled_pixels rows
         * 0..(active_native * scale - 1) where active_native =
         * _native_height - _lb_top_applied - _lb_bot_applied.  Bound
         * fb_row to that same range so we don't blit stale data from
         * _scaled_pixels rows that Pass 1 didn't touch this frame. */
        int active_native_h = _native_height - _lb_top_applied - _lb_bot_applied;
        int active_fb_h     = active_native_h * scale;
        for (ty = 0; ty < _tiles_y; ty++) {
            unsigned char *trow = _dirty_tiles + ty * _tiles_x;
            int tx_first = -1, tx_scan;
            for (tx_scan = 0; tx_scan < _tiles_x; tx_scan++) {
                if (trow[tx_scan]) { tx_first = tx_scan; break; }
            }
            if (tx_first < 0) continue;

            /* Map this NATIVE-source tile_row to a destination fb_row
             * range, accounting for the letterbox skip.  Tiles entirely
             * inside the skipped letterbox produce no fb writes. */
            int src_row_start = ty * TILE_H;
            int src_row_end   = src_row_start + TILE_H;
            if (src_row_end > _native_height) src_row_end = _native_height;
            int eff_src_start = src_row_start;
            int eff_src_end   = src_row_end;
            if (eff_src_start < _lb_top_applied) eff_src_start = _lb_top_applied;
            if (eff_src_end > _native_height - _lb_bot_applied)
                eff_src_end = _native_height - _lb_bot_applied;
            if (eff_src_start >= eff_src_end) continue;
            int fb_row_start = (eff_src_start - _lb_top_applied) * scale;
            int fb_row_end   = (eff_src_end   - _lb_top_applied) * scale;
            if (fb_row_end > active_fb_h) fb_row_end = active_fb_h;

            if (batchedBlits) {
                // The driver's aligned ldd/std path requires aligned strides
                // when copying multiple rows. Otherwise retain single rows.
                const int stripRows=((fb_stride | _scaled_pitch)&7) ? 1 : 8;
                SparcFbBlit::tileRow(trow, _tiles_x, TILE_W, _native_width, scale,
                    fb_row_start, fb_row_end, stripRows,
                    [&](int x, int y, int width, int height) {
                        sparc_fb_ldd_copy(fb_base+y*fb_stride+x, fb_stride,
                            _scaled_pixels+y*_scaled_pitch+x, _scaled_pitch, width, height);
                    }, [&]() { if (!sst_yield_off) sched_yield(); });
            } else {
            int fb_row;
            for (fb_row = fb_row_start; fb_row < fb_row_end; fb_row++) {
                unsigned char *fb_dst_row = fb_base + fb_row * fb_stride;
                byte *scaled_src_row = _scaled_pixels + fb_row * _scaled_pitch;
                int tx;
                for (tx = tx_first; tx < _tiles_x; tx++) {
                    if (!trow[tx]) continue;
                    int x0 = tx * TILE_W;
                    int x1 = x0 + TILE_W;
                    if (x1 > _native_width) x1 = _native_width;
                    int seg_dst_off = x0 * scale;
                    int seg_dst_w = (x1 - x0) * scale;
                    sparc_fb_ldd_copy(fb_dst_row + seg_dst_off, fb_stride,
                                      scaled_src_row + seg_dst_off, 0,
                                      seg_dst_w, 1);
                }
                if (!sst_yield_off && (fb_row & 7) == 0) sched_yield();
            }
            }
        }
    }

    /* Snapshot just-blit tiles into _last_native_pixels so next frame's
     * lockScreen-diff pass has a fresh baseline.  Only dirty tiles get
     * copied (clean tiles haven't changed, so the snapshot stays valid).
     * This caps the snapshot RAM cost at K*256 bytes per frame instead
     * of a flat 76KB, which matters when most tiles are clean. */
    if (_dirty_tiles && _last_native_pixels) {
        int ty;
        for (ty = 0; ty < _tiles_y; ty++) {
            unsigned char *trow = _dirty_tiles + ty * _tiles_x;
            int row_start = ty * TILE_H;
            int row_end = row_start + TILE_H;
            if (row_end > _native_height) row_end = _native_height;
            int tx;
            for (tx = 0; tx < _tiles_x; tx++) {
                if (!trow[tx]) continue;
                int x0 = tx * TILE_W;
                int x1 = x0 + TILE_W;
                if (x1 > _native_width) x1 = _native_width;
                int seg_w = x1 - x0;
                int row;
                for (row = row_start; row < row_end; row++) {
                    memcpy(_last_native_pixels + row * _native_pitch + x0,
                           _native_pixels      + row * _native_pitch + x0, seg_w);
                }
            }
        }
        _last_native_valid = true;
    }

    /* Clear dirty bits for next frame.  Done last so the diagnostic
     * counter above sees the actual dirty set we just blit. */
    if (_dirty_tiles) memset(_dirty_tiles, 0, _tiles_x * _tiles_y);

    if (_sparcfb_initialized) {
        /* DIAGNOSTIC: on every 100th frame, dump _scaled_pixels AND the
         * fb region we just wrote to.  Opt-in via SPARCFB_DUMP=1. */
        if (sst_dump_on) {
            static int dn = 0;
            if ((dn % 100) == 1) {
                FILE *f = fopen("/tmp/scaled_pixels.raw", "wb");
                if (f) { fwrite(_scaled_pixels, 1, _scaled_pitch * _scaled_height, f); fclose(f); }
                f = fopen("/tmp/fb_region.raw", "wb");
                if (f) {
                    unsigned char *fbp = (unsigned char *)_sparcfb_ctx->fb_memory;
                    int fbst = _sparcfb_ctx->fb_linebytes;
                    int row;
                    for (row = 0; row < _scaled_height; row++) {
                        fwrite(fbp + (_fb_offset_y + row) * fbst + _fb_offset_x,
                               1, _scaled_width, f);
                    }
                    fclose(f);
                }
                f = fopen("/tmp/fb_full.raw", "wb");
                if (f) {
                    fwrite(_sparcfb_ctx->fb_memory, 1, _sparcfb_ctx->fb_size, f);
                    fclose(f);
                }
                warning("dumps written: scaled %dx%d pitch=%d, fb_full size=%d",
                        _scaled_width, _scaled_height, _scaled_pitch,
                        _sparcfb_ctx->fb_size);
            }
            dn++;
        }
    }

    // Opt-in per-frame backend timing for matched hardware trials.
    static const bool profileBlit = (getenv("SPARCFB_PROFILE_BLIT") != NULL);
    if (profileBlit) {
        static unsigned long sumUs=0, frames=0;
        gettimeofday(&frame_end, NULL);
        const long us=(frame_end.tv_sec-frame_start.tv_sec)*1000000L+
                      frame_end.tv_usec-frame_start.tv_usec;
        if (us>=0) sumUs+=us;
        if (++frames==50) {
            warning("SST blit batch=%d packed=%d direct=%d frames=50 us_per_frame=%lu", batchedBlits, packedScale, direct_blit, sumUs/50);
            frames=0; sumUs=0;
        }
    }

    /* Running sum of dirty tile counts since last fps report — averaged
     * into the fps log line so we can see whether dirty rects are
     * actually saving work in steady state. */
    static long dirty_accum = 0;
    if (sst_debug_dirty) dirty_accum += dirty_tile_count;

    /* FPS reporting every 100 frames — opt-in via SPARCFB_FPS=1. */
    static const bool sst_log_fps = (getenv("SPARCFB_FPS") != NULL);
    if (sst_log_fps && (frame_count % 100) == 0) {
        gettimeofday(&frame_end, NULL);
        long elapsed_us = (frame_end.tv_sec - last_report.tv_sec) * 1000000L
                        + (frame_end.tv_usec - last_report.tv_usec);
        if (elapsed_us > 0) {
            int total_tiles = _tiles_x * _tiles_y;
            if (sst_debug_dirty) {
                warning("sparcfb-gfx-noX: %.1f fps (scale=%d, avg %ld/%d dirty tiles, %.0f%%)",
                        100.0 * 1000000.0 / (double)elapsed_us, _scale_factor,
                        dirty_accum / 100, total_tiles,
                        100.0 * (double)(dirty_accum / 100) / (double)total_tiles);
            } else {
                warning("sparcfb-gfx-noX: %.1f fps (native 8-bit, scale=%d)",
                        100.0 * 1000000.0 / (double)elapsed_us, _scale_factor);
            }
        }
        last_report = frame_end;
        dirty_accum = 0;
    }
}

void SparcFbGraphicsManager::compositeNativeCursor() {
    if (!_native_cursor_visible || !_native_cursor_pixels || !_scaled_pixels)
        return;

    int scale = _scale_factor;
    int draw_x = (_native_mouse_x - _native_cursor_hotx) * scale;
    /* draw_y in scaled coords, accounting for any letterbox skip — the
     * scaled buffer's row 0 is content-row _lb_top_applied of native. */
    int draw_y = ((_native_mouse_y - _native_cursor_hoty) - _lb_top_applied) * scale;
    int cursor_row, cursor_col;

    for (cursor_row = 0; cursor_row < _native_cursor_h; cursor_row++) {
        for (cursor_col = 0; cursor_col < _native_cursor_w; cursor_col++) {
            byte pixel = _native_cursor_pixels[cursor_row * _native_cursor_w + cursor_col];
            if ((uint32)pixel == _native_cursor_keycolor) continue;

            int base_x = draw_x + cursor_col * scale;
            int base_y = draw_y + cursor_row * scale;
            int sr, sc;

            for (sr = 0; sr < scale; sr++) {
                int dy = base_y + sr;
                if (dy < 0 || dy >= _scaled_height) continue;
                for (sc = 0; sc < scale; sc++) {
                    int dx = base_x + sc;
                    if (dx < 0 || dx >= _scaled_width) continue;
                    _scaled_pixels[dy * _scaled_pitch + dx] = pixel;
                }
            }
        }
    }
}

/* ================================================================
 * Mouse cursor
 * ================================================================ */

bool SparcFbGraphicsManager::showMouse(bool visible) {
    /* Always forward to parent so its cursor-visibility state stays in
     * sync — the overlay path (parent's updateScreen + cursor
     * compositing) renders the cursor based on parent's flag, and if we
     * intercept showMouse without telling parent, the dialog cursor
     * goes invisible. */
    bool prev_parent = SurfaceSdlGraphicsManager::showMouse(visible);
    if (_native_mode) {
        bool was = _native_cursor_visible;
        _native_cursor_visible = visible;
        return was;
    }
    return prev_parent;
}

void SparcFbGraphicsManager::warpMouse(int x, int y) {
    static const bool sst_debug_mouse = (getenv("SPARCFB_DEBUG_MOUSE") != NULL);
    if (_native_mode) {
        _native_mouse_x = x;
        _native_mouse_y = y;
    }
    /* Forward to parent so its _cursorX/_cursorY tracking + the SDL
     * fakeWarpMouse stay coherent (notifyMousePosition reads parent
     * state for active-area checks). */
    SurfaceSdlGraphicsManager::warpMouse(x, y);
    if (sst_debug_mouse) {
        static int wm_n = 0;
        if (wm_n < 30 || (wm_n % 50) == 0) {
            warning("warpMouse #%d -> (%d,%d) native_mode=%d", wm_n, x, y, _native_mode ? 1 : 0);
        }
        wm_n++;
    }
}

bool SparcFbGraphicsManager::notifyMousePosition(Common::Point &mouse) {
    // The framebuffer SDL driver already reports logical viewport coordinates.
    // Native presentation bypasses SDL's aspect/DPI geometry; input must too.
    // Overlay input continues through the parent's window conversion.
    static const bool sst_debug_mouse = (getenv("SPARCFB_DEBUG_MOUSE") != NULL);
    if (SDL_GetHintBoolean("SDL_SPARCFB_LAUNCHER", SDL_FALSE) &&
        (getenv("SPARCFB_GUI_BENCH") || getenv("SPARCFB_GUI_WIDGET_TEST"))) {
        static bool geometryReported;
        if (!geometryReported) {
            const Common::Point a = convertWindowToVirtual(0, 0);
            const Common::Point b = convertWindowToVirtual(_windowWidth / 2, _windowHeight / 2);
            const Common::Point c = convertWindowToVirtual(_windowWidth - 1, _windowHeight - 1);
            const bool identity = _windowWidth == _videoMode.overlayWidth &&
                _windowHeight == _videoMode.overlayHeight && a.x == 0 && a.y == 0 &&
                b.x == _windowWidth / 2 && b.y == _windowHeight / 2 &&
                c.x == _windowWidth - 1 && c.y == _windowHeight - 1;
            debug(1, "TCX GUI mapping: identity=%d window=%dx%d overlay=%dx%d", identity,
                _windowWidth, _windowHeight, _videoMode.overlayWidth, _videoMode.overlayHeight);
            geometryReported = true;
        }
    }
    int in_x = mouse.x, in_y = mouse.y;
    bool valid;
    if (_native_mode && !isOverlayVisible()) {
        mouse.x = CLIP<int>(mouse.x, 0, _videoMode.screenWidth - 1);
        mouse.y = CLIP<int>(mouse.y, 0, _videoMode.screenHeight - 1);
        const Common::Point windowCursor = convertVirtualToWindow(mouse.x, mouse.y);
        setMousePosition(windowCursor.x, windowCursor.y);
        _cursorLastInActiveArea = true;
        showSystemMouseCursor(false);
        valid = true;
    } else {
        valid = SurfaceSdlGraphicsManager::notifyMousePosition(mouse);
    }
    if (_native_mode && valid) {
        _native_mouse_x = mouse.x;
        _native_mouse_y = mouse.y;
    }
    if (sst_debug_mouse) {
        static int nmp_n = 0;
        if (nmp_n == 0) {
            warning("notifyMousePosition GEOM: drawRect=(%d,%d,%d,%d) active=%dx%d "
                    "window=%dx%d screen=%dx%d scale=%d aspect=%d overlay=%dx%d overlayInGUI=%d",
                    _activeArea.drawRect.left, _activeArea.drawRect.top,
                    _activeArea.drawRect.right, _activeArea.drawRect.bottom,
                    _activeArea.width, _activeArea.height,
                    _windowWidth, _windowHeight,
                    _videoMode.screenWidth, _videoMode.screenHeight,
                    _videoMode.scaleFactor,
                    _videoMode.aspectRatioCorrection ? 1 : 0,
                    _videoMode.overlayWidth, _videoMode.overlayHeight,
                    _overlayInGUI ? 1 : 0);
        }
        if (nmp_n < 30 || (nmp_n % 50) == 0) {
            warning("notifyMousePosition #%d in=(%d,%d) valid=%d -> virt=(%d,%d) native_xy=(%d,%d)",
                    nmp_n, in_x, in_y, valid ? 1 : 0,
                    mouse.x, mouse.y, _native_mouse_x, _native_mouse_y);
        }
        nmp_n++;
    }
    return valid;
}

void SparcFbGraphicsManager::setMouseCursor(const void *buf, uint w, uint h,
                                              int hotspotX, int hotspotY,
                                              uint32 keycolor, bool dontScale,
                                              const Graphics::PixelFormat *format,
                                              const byte *mask) {
    /* Store for native compositing */
    delete[] (_native_cursor_pixels);
    _native_cursor_w = w;
    _native_cursor_h = h;
    _native_cursor_hotx = hotspotX;
    _native_cursor_hoty = hotspotY;
    _native_cursor_keycolor = keycolor;

    int cursor_size = w * h;
    _native_cursor_pixels = new byte[cursor_size];
    if (_native_cursor_pixels && buf)
        memcpy(_native_cursor_pixels, buf, cursor_size);

    /* Also update parent for overlay mode */
    SurfaceSdlGraphicsManager::setMouseCursor(buf, w, h, hotspotX, hotspotY,
                                                keycolor, dontScale, format, mask);
}
