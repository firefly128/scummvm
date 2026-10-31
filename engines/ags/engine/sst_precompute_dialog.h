/* SST AGS asset-precompute progress dialog.
 *
 * Modeled on engines/grim/md5checkdialog.{h,cpp}: a modal Dialog that
 * spins on handleTickle() to do background work in 1-sprite chunks
 * while keeping the GUI overlay responsive.
 *
 * Now runs in two passes when the game has no _palette.bin yet:
 *   Pass 1 (kPhaseSample):
 *     - Walk all sprite slots, load each one into memory.
 *     - The initialize_sprite hook feeds the raw >8bpp pixels into
 *       SSTPaletteGen::SamplePixels(), building a frequency histogram.
 *     - SSTAssetCache::Store is suppressed so we don't write .pal8 files
 *       quantized against the wrong (default cube) palette.
 *     - At end: SSTPaletteGen::Generate() produces a game-tuned 256-colour
 *       palette, SSTAssetCache::SavePalette() persists it, and the live
 *       _G(current_palette) is reloaded via applyPalette().
 *   Pass 2 (kPhaseQuantize):
 *     - Walk all sprite slots again.
 *     - This time Store is re-enabled so .pal8 files are written.  Each
 *       sprite is quantized against the new game-tuned palette via the
 *       existing surface.cpp sst_quantize_to_8bit path.
 *
 * On subsequent launches, _palette.bin exists, the sample pass is
 * skipped, and only the quantize pass runs (and even it is a no-op when
 * the cache is complete). */

#ifndef AGS_ENGINE_SST_PRECOMPUTE_DIALOG_H
#define AGS_ENGINE_SST_PRECOMPUTE_DIALOG_H

#include "gui/dialog.h"

namespace Graphics { struct Surface; }

namespace GUI {
class SliderWidget;
class StaticTextWidget;
}

namespace AGS3 {
namespace AGS {
namespace Engine {

/* Shared native screen for graphics preparation, source verification and
 * chunked audio decoding. Owns and restores only the display palette. */
class SSTPreparationScreen {
public:
	SSTPreparationScreen();
	~SSTPreparationScreen();
	bool update(const char *phase, int done, int total, const char *detail = "", bool force = false);
private:
	::Graphics::Surface *_screen;
	byte _savedPalette[768], _progressPalette[768];
	uint32 _lastDraw;
	bool _first;
};

/* Prepare separate, content-addressed PCM files; never rewrite a VOX. */
bool PrepareSSTAudioCache();

class SSTPrecomputeDialog : public ::GUI::Dialog {
public:
	enum Phase {
		kPhaseRooms,    /* sample one room per GUI tick */
		kPhaseSample,   /* walk + histogram, suppress Store */
		kPhaseQuantize  /* walk + quantize (existing behaviour) */
	};

	SSTPrecomputeDialog(int total_sprites, int pending, Phase phase);

	bool runPreparation();

	bool HasWork() const { return _total > 0; }

protected:
	void handleTickle() override;
	/* Do not let Escape commit an incomplete palette sample. */
	void handleKeyDown(Common::KeyState state) override {}

private:
	void advanceOne();

	Phase _phase;
	int _total;
	int _done;
	int _next_index;
	int _max_index;
	::GUI::SliderWidget    *_bar;
	::GUI::StaticTextWidget *_status;
};

/* Top-level entry point.  Runs sample-then-quantize on first launch (no
 * _palette.bin); on subsequent launches runs quantize-only.  Returns
 * true if any work was done. */
bool RunPrecomputePass();

} // namespace Engine
} // namespace AGS
} // namespace AGS3

#endif
