/*
 * sparcfb-graphics-noX.h — Native 8-bit graphics backend, X11-free.
 *
 * Subclasses SurfaceSdlGraphicsManager. When a game engine starts
 * (initSize is called), switches to native 8-bit rendering: writes
 * directly to /dev/fb via sparc_fb library, pushes palette to BT458
 * RAMDAC via sparc_fb_set_palette (no X11 colormap), composites mouse
 * cursor as an INDEX8 sprite. The launcher/overlay still uses the
 * parent SDL path.
 *
 * Differences vs the X11 variant: no XOpenDisplay / XCreateColormap /
 * XStoreColors / XGrabPointer / XCreatePixmapCursor. SDL2 sparcfb video
 * driver handles input; our backend handles output. They share /dev/fb
 * by mmap'ing it independently — no contention because the SDL_sparcfb
 * driver checks SDL_GetHint("SDL_SPARCFB_NATIVE") and stands down when
 * we set it.
 *
 * Part of the Sunstorm Project
 */
#ifndef SPARCFB_GRAPHICS_NOX_H
#define SPARCFB_GRAPHICS_NOX_H

#include "backends/graphics/surfacesdl/surfacesdl-graphics.h"

/* Forward-declare sparc_fb context — full definition in the .cpp */
struct sparc_fb_context_tag;
typedef struct sparc_fb_context_tag sparc_fb_context_fwd;

class SparcFbGraphicsManager : public SurfaceSdlGraphicsManager {
public:
    SparcFbGraphicsManager(SdlEventSource *sdlEventSource, SdlWindow *window);
    virtual ~SparcFbGraphicsManager();

    /* Override the rendering hot path */
    void initSize(uint width, uint height, const Graphics::PixelFormat *format = NULL) override;
    void setPalette(const byte *colors, uint start, uint num) override;
    void copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) override;
    Graphics::Surface *lockScreen() override;
    void unlockScreen() override;
    void fillScreen(uint32 col) override;
    void fillScreen(const Common::Rect &r, uint32 col) override;
    void updateScreen() override;

    /* Overlay routing: when scummvm shows the launcher / save dialog
     * overlay, route updates through the SDL path so SDL_sparcfb
     * actually paints them to /dev/fb (we normally suppress that with
     * the SDL_SPARCFB_NATIVE hint to avoid a double-write while the
     * native pipeline is rendering game frames). */
    void showOverlay(bool inGUI) override;
    void hideOverlay() override;

    /* Override mouse to fix coordinate transform */
    bool showMouse(bool visible) override;
    void warpMouse(int x, int y) override;
    bool notifyMousePosition(Common::Point &mouse) override;
    void setMouseCursor(const void *buf, uint w, uint h,
                        int hotspotX, int hotspotY, uint32 keycolor,
                        const Graphics::PixelFormat *format = nullptr,
                        const byte *mask = nullptr,
                        frac_t scaleX = FRAC_ONE, frac_t scaleY = FRAC_ONE) override;

    /* Override palette queries */
    void grabPalette(byte *colors, uint start, uint num) const override;

protected:
    /* Override the internal update to bypass SDL renderer */
    void internUpdateScreen() override;

private:
    /* sparc_fb context for direct /dev/fb writes (opaque, allocated in .cpp) */
    void *_sparcfb_context_ptr;
    bool _sparcfb_initialized;
    int _launcher_width, _launcher_height;

    /* Game shadow buffer — native 8-bit indexed, written by engine */
    byte *_native_pixels;
    int _native_width;
    int _native_height;
    int _native_pitch;
    Graphics::Surface _native_surface;  /* for lockScreen */

    /* Scaled output buffer */
    byte *_scaled_pixels;
    int _scaled_width;
    int _scaled_height;
    int _scaled_pitch;
    int _scale_factor;

    /* Centering on physical FB */
    int _fb_offset_x;
    int _fb_offset_y;

    /* Direct palette — game's actual 256-color palette */
    byte _native_palette[256 * 3];
    bool _native_palette_dirty;
    bool _native_margins_dirty;
    byte _native_margin_color;

    /* Track whether we're in native (game) or SDL (overlay/launcher) mode */
    bool _native_mode;

    /* Cursor state for native mode */
    byte *_native_cursor_pixels;
    int _native_cursor_w;
    int _native_cursor_h;
    int _native_cursor_hotx;
    int _native_cursor_hoty;
    frac_t _native_cursor_scale_x, _native_cursor_scale_y;
    uint32 _native_cursor_keycolor;
    bool _native_cursor_visible;

    /* Mouse position in game coords */
    int _native_mouse_x;
    int _native_mouse_y;

    /* Frame skip: cached copy of last frame's _native_pixels.  scaleAndBlit
     * compares the current frame to this cache; if identical (no engine
     * pixel changes since last present), the entire MMIO blit is skipped. */
    byte *_last_native_pixels;
    bool _last_native_valid;

    /* Tile-level dirty-rect tracking — split _native_pixels into 16×16
     * tiles and only blit tiles whose pixels actually changed since the
     * last present.  TILE_W/H == 16 keeps each tile_row's MMIO span
     * (16*scale dst bytes, max 48 at scale=3) divisible by 8 so
     * sparc_fb_ldd_copy() stays on its fast path.  Marked dirty in
     * copyRectToScreen + lockScreen + fillScreen + initSize, plus the
     * cursor's old & new bbox each frame in scaleAndBlit. */
    static const int TILE_W = 16;
    static const int TILE_H = 16;
    unsigned char *_dirty_tiles;   /* 1 byte per tile, 0 = clean, 1 = dirty */
    int _tiles_x;
    int _tiles_y;
    bool _dirty_disabled;          /* SPARCFB_NO_DIRTY=1 forces full blit */
    bool _lockscreen_called;       /* engine grabbed lockScreen this frame —
                                    * we can't see writes through that surface,
                                    * so diff per-tile against the snapshot
                                    * stored in _last_native_pixels rather
                                    * than blanket-marking all dirty */

    /* Cursor bbox tracking — needed so the previous cursor position gets
     * cleared (overwritten with game pixels) the frame after the cursor
     * moves, even if no engine copyRectToScreen touched the area. */
    int _last_cursor_drawn_x, _last_cursor_drawn_y;
    int _last_cursor_drawn_w, _last_cursor_drawn_h;
    bool _was_cursor_visible;

    /* Letterbox detection — stripped contiguous all-zero rows on top
     * and bottom of _native_pixels.  Detected per-frame (cheap spot
     * check), held stable across frames before re-windowing.  *_applied
     * are the values currently reflected in the THC active region; the
     * blit loop skips native rows in [0, _lb_top_applied) and
     * [native_h - _lb_bot_applied, native_h) and shifts the dest VRAM
     * row by -_lb_top_applied, so the actually-drawn content fills the
     * windowed VRAM area starting at row 0. */
    int  _lb_top_detected;
    int  _lb_bot_detected;
    int  _lb_top_applied;
    int  _lb_bot_applied;
    int  _lb_stable_frames;
    int  _current_win_h;        /* currently-applied THC win_height; 0 = no
                                 * windowing active */

    /* Helpers */
    void pushNativePalette();
    void repaintNativeMargins();
    bool syncPaletteFromParent();
    void scaleAndBlit();
    void compositeNativeCursor();
    void markTilesDirtyRect(int x, int y, int w, int h);
    void markAllTilesDirty();
};

#endif /* SPARCFB_GRAPHICS_NOX_H */
