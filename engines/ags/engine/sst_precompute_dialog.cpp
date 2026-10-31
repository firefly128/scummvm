/* SST AGS asset-precompute progress dialog.  Two-phase: sample then
 * quantize.  See sst-ags-precompute-dialog.h for the phase contract. */

#include "ags/engine/sst_precompute_dialog.h"
#include "ags/shared/ac/sst_asset_cache.h"
#include "ags/shared/ac/sst_palette_gen.h"
#include "ags/shared/ac/sprite_cache.h"
#include "ags/shared/gfx/bitmap.h"
#include "ags/globals.h"

#include "common/system.h"
#include "common/translation.h"
#include "common/events.h"
#include "common/textconsole.h"
#include "engines/engine.h"
#include "graphics/fontman.h"
#include "graphics/font.h"
#include "graphics/paletteman.h"
#include "graphics/surface.h"

#include "gui/gui-manager.h"
#include "gui/ThemeEval.h"
#include "gui/widget.h"

namespace AGS3 {

/* Forward declarations of the cube-palette helpers from color.cpp.
 * RunPrecomputePass calls sst_resolve_cube_mode() first to make sure
 * sst_cube_palette_active is populated from SPARCFB_CUBE_PALETTE before
 * we decide whether to run the sample pass — engine_init_sprites can
 * fire before AGS's first applyPalette(), so the lazy resolution in
 * applyPalette would otherwise leave the flag false here. */
extern bool sst_cube_palette_active;
void sst_resolve_cube_mode();
void sst_install_palette_from_file_or_cube();

namespace AGS {
namespace Engine {

/* Install after the user cache is initialized, even when palette mode
 * was resolved earlier during font/graphics setup. */
static void InstallPreparedPalette() {
	sst_install_palette_from_file_or_cube();
	if (g_system->getScreenFormat().bytesPerPixel != 1) return;
	byte rgb[768];
	for (int i = 0; i < 256; ++i) {
		rgb[i*3] = _G(current_palette)[i].r * 255 / 63;
		rgb[i*3+1] = _G(current_palette)[i].g * 255 / 63;
		rgb[i*3+2] = _G(current_palette)[i].b * 255 / 63;
	}
	g_system->getPaletteManager()->setPalette(rgb, 0, 256);
}

/* Count sprites that exist in the .ags spriteset but lack a .pal8 file. */
static int CountPending(int max_index) {
	if (!AGS::Shared::SSTAssetCache::IsEnabled()) return 0;
	int pending = 0;
	for (int i = 1; i < max_index; i++) {
		if (!_GP(spriteset).DoesSpriteExist(i))
			continue;
		if (!AGS::Shared::SSTAssetCache::HasEntry(i))
			pending++;
	}
	return pending;
}

/* Count all sprites that exist in the .ags spriteset (used to size the
 * sample-pass bar — we walk every sprite, not just uncached ones). */
static int CountAllExisting(int max_index) {
	int n = 0;
	for (int i = 1; i < max_index; i++) {
		if (_GP(spriteset).DoesSpriteExist(i)) n++;
	}
	return n;
}

SSTPrecomputeDialog::SSTPrecomputeDialog(int total_sprites, int pending, Phase phase)
		: ::GUI::Dialog(0, 0, 200, 80),
		  _phase(phase),
		  _total(pending),
		  _done(0),
		  _next_index(phase == kPhaseRooms ? 0 : 1),
		  _max_index(total_sprites),
		  _bar(nullptr),
		  _status(nullptr) {

	const int screenW = g_system->getOverlayWidth();
	const int screenH = g_system->getOverlayHeight();
	_w = (screenW < 280 + 20) ? (screenW - 20) : 280;
	_h = 80;
	_x = (screenW - _w) / 2;
	_y = (screenH - _h) / 2;

	warning("SST prepare phase=%d total=%d overlay=%dx%d dialog=%d,%d %dx%d", (int)_phase, _total, screenW, screenH, _x, _y, _w, _h);

	const char *title = (_phase == kPhaseRooms)
		? "Preparing room backgrounds..."
		: (_phase == kPhaseSample)
		? "Building game palette..."
		: "Preparing game graphics...";

	int innerW = _w - 20;
	int y = 6;
	new ::GUI::StaticTextWidget(this, 10, y, innerW, kLineHeight,
		Common::U32String(title),
		Graphics::kTextAlignCenter);
	y += kLineHeight + 4;
	_status = new ::GUI::StaticTextWidget(this, 10, y, innerW, kLineHeight,
		Common::U32String("Starting preparation..."), Graphics::kTextAlignCenter);
	y += kLineHeight + 4;
	_bar = new ::GUI::SliderWidget(this, 10, y, innerW, 8);
	_bar->setMinValue(0);
	_bar->setMaxValue(_total > 0 ? _total : 1);
	_bar->setValue(0);
	y += 12;
	new ::GUI::StaticTextWidget(this, 10, y, innerW, kLineHeight,
		Common::U32String("First launch may take a while."), Graphics::kTextAlignCenter);
}

SSTPreparationScreen::SSTPreparationScreen() : _screen(new ::Graphics::Surface()), _lastDraw(0), _first(true) {
	_screen->create(g_system->getWidth(), g_system->getHeight(), g_system->getScreenFormat());
	g_system->getPaletteManager()->grabPalette(_savedPalette, 0, 256);
	for (int i = 0; i < 256; ++i)
		_progressPalette[i * 3] = _progressPalette[i * 3 + 1] = _progressPalette[i * 3 + 2] = i;
}

SSTPreparationScreen::~SSTPreparationScreen() {
	g_system->getPaletteManager()->setPalette(_savedPalette, 0, 256);
	_screen->free();
	delete _screen;
}

bool SSTPreparationScreen::update(const char *phase, int done, int total, const char *detail, bool force) {
	Common::Event event;
	while (g_system->getEventManager()->pollEvent(event)) {
		if (event.type == Common::EVENT_QUIT) ::Engine::quitGame();
	}
	if (g_system->getEventManager()->shouldQuit()) return false;
	uint32 now = g_system->getMillis();
	if (!_first && !force && now - _lastDraw < 100) return true;
	_first = false;
	_lastDraw = now;
	int width = _screen->w, height = _screen->h;
	const ::Graphics::Font *font = FontMan.getFontByUsage(::Graphics::FontManager::kGUIFont);
	_screen->fillRect(Common::Rect(width, height), 0);
	int y = MAX(4, height / 2 - 40);
	font->drawString(_screen, Common::String(phase), 4, y, width - 8, 255, ::Graphics::kTextAlignCenter);
	y += font->getFontHeight() + 6;
	font->drawString(_screen, Common::String::format("%d / %d", done, total), 4, y, width - 8, 255, ::Graphics::kTextAlignCenter);
	y += font->getFontHeight() + 6;
	int left = 16, right = width - 16;
	_screen->fillRect(Common::Rect(left, y, right, y + 12), 255);
	_screen->fillRect(Common::Rect(left + 1, y + 1, right - 1, y + 11), 0);
	int filled = total > 0 ? (int)((int64)(right - left - 4) * done / total) : 0;
	if (filled > 0) _screen->fillRect(Common::Rect(left + 2, y + 2, left + 2 + filled, y + 10), 200);
	font->drawString(_screen, Common::String(detail), 4, y + 16, width - 8, 255, ::Graphics::kTextAlignCenter, 0, true);
	font->drawString(_screen, Common::String("First launch may take a while."), 4, y + 32, width - 8, 255, ::Graphics::kTextAlignCenter);
	g_system->getPaletteManager()->setPalette(_progressPalette, 0, 256);
	g_system->copyRectToScreen(_screen->getPixels(), _screen->pitch, 0, 0, width, height);
	g_system->updateScreen();
	return true;
}

/* Native preparation avoids the legacy SDL overlay/viewport mismatch. */
bool SSTPrecomputeDialog::runPreparation() {
	if (g_system->getScreenFormat().bytesPerPixel != 1) return runModal() == 1;
	SSTPreparationScreen screen;
	const char *title = _phase == kPhaseRooms ? "Reading room backgrounds"
		: _phase == kPhaseSample ? "Sampling original graphics" : "Caching game graphics";
	for (;;) {
		if (!screen.update(title, _done, _total, "", _done >= _total)) return false;
		if (_done >= _total) return true;
		advanceOne();
		g_system->delayMillis(1);
	}
}

void SSTPrecomputeDialog::handleTickle() {
	if (_done >= _total) {
		setResult(1);
		close();
		return;
	}
	advanceOne();
}

void SSTPrecomputeDialog::advanceOne() {
	if (_phase == kPhaseRooms) {
		AGS::Shared::SSTPaletteGen::SampleRoomBackground(_next_index++);
		++_done;
		char status[80];
		snprintf(status, sizeof(status), "Checking room backgrounds (%d / %d)", _done, _total);
		_status->setLabel(Common::U32String(status));
		_status->markAsDirty();
		_bar->setValue(_done);
		_bar->markAsDirty();
		return;
	}
	while (_next_index < _max_index) {
		int idx = _next_index++;
		if (!_GP(spriteset).DoesSpriteExist(idx))
			continue;

		if (_phase == kPhaseQuantize && AGS::Shared::SSTAssetCache::HasEntry(idx))
			continue;

		if (_phase == kPhaseSample)
			AGS::Shared::SSTAssetCache::InvalidateEntry(idx);
		_GP(spriteset).PrecacheSprite(idx);
		_GP(spriteset).DisposeCached(idx);
		if (_phase == kPhaseQuantize && !AGS::Shared::SSTAssetCache::HasEntry(idx))
			error("Cannot write graphics cache for sprite %d; check space and permissions", idx);

		_done++;

		char buf[64];
		const char *fmt = (_phase == kPhaseSample)
			? "sampling sprite %d  (%d / %d)"
			: "quantizing sprite %d  (%d / %d)";
		snprintf(buf, sizeof(buf), fmt, idx, _done, _total);
		_status->setLabel(Common::U32String(buf));
		_bar->setValue(_done);
		_bar->markAsDirty();
		_status->markAsDirty();
		return;
	}

	/* Walked off the end. */
	_done = _total;
	_bar->setValue(_total);
	_bar->markAsDirty();
}

bool RunPrecomputePass() {
	if (!AGS::Shared::SSTAssetCache::IsEnabled())
		return false;

	if (g_system->getScreenFormat().bytesPerPixel == 1 && !PrepareSSTAudioCache())
		return false;

	/* Resolve SPARCFB_CUBE_PALETTE before deciding whether to sample.
	 * applyPalette() may not have run yet, so the cached flag could
	 * still be false even when the user requested cube mode. */
	sst_resolve_cube_mode();
	InstallPreparedPalette();
	if (!AGS::Shared::SSTAssetCache::RequiresPaletteConversion() && !sst_cube_palette_active)
		return true; // Original indexed games need no graphics conversion pass.

	int max_index = (int)_GP(spriteset).GetSpriteSlotCount();

	/* Phase 1 (sample) — only when we have no game-tuned palette yet. */
	if (sst_cube_palette_active && !AGS::Shared::SSTAssetCache::HasPaletteFile()) {
		int existing = CountAllExisting(max_index);
		if (existing > 0) {
			AGS::Shared::SSTPaletteGen::Reset();
			AGS::Shared::SSTPaletteGen::SetActive(true);
			AGS::Shared::SSTAssetCache::SetStoreSuppressed(true);

			/* Show progress before loading any room; return to the GUI
			 * between rooms rather than blocking behind a black screen. */
			int rooms = AGS::Shared::SSTPaletteGen::RoomBackgroundSlotCount();
			SSTPrecomputeDialog dlg_rooms(rooms, rooms, SSTPrecomputeDialog::kPhaseRooms);
			if (!dlg_rooms.runPreparation()) {
				AGS::Shared::SSTPaletteGen::SetActive(false);
				AGS::Shared::SSTAssetCache::SetStoreSuppressed(false);
				return false;
			}

			SSTPrecomputeDialog dlg_sample(max_index, existing, SSTPrecomputeDialog::kPhaseSample);
			if (!dlg_sample.runPreparation()) {
				AGS::Shared::SSTPaletteGen::SetActive(false);
				AGS::Shared::SSTAssetCache::SetStoreSuppressed(false);
				return false;
			}

			AGS::Shared::SSTPaletteGen::SetActive(false);
			AGS::Shared::SSTAssetCache::SetStoreSuppressed(false);

			byte rgb[768];
			AGS::Shared::SSTPaletteGen::Generate(rgb);
			AGS::Shared::SSTAssetCache::SavePalette(rgb);

			/* Reinstall the (now game-tuned) palette into _G(current_palette)
			 * and push it to the framebuffer. */
			InstallPreparedPalette();
		}
	}

	if (!AGS::Shared::SSTAssetCache::EnsureColourLookup()) return false;

	/* Phase 2 (quantize) — existing behaviour. */
	int pending = CountPending(max_index);
	if (pending == 0)
		return false;

	SSTPrecomputeDialog dlg_quant(max_index, pending, SSTPrecomputeDialog::kPhaseQuantize);
	return dlg_quant.runPreparation();
}

} // namespace Engine
} // namespace AGS
} // namespace AGS3
