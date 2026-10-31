#include "ags/shared/ac/sst_palette_gen.h"
#include "ags/shared/ac/common_defines.h"
#include "ags/shared/ac/game_setup_struct.h"
#include "ags/shared/core/asset_manager.h"
#include "ags/shared/game/room_struct.h"
#include "ags/shared/gfx/bitmap.h"
#include "ags/shared/debugging/out.h"
#include "ags/lib/allegro/color.h"
#include "ags/globals.h"

#include "common/scummsys.h"

#include <string.h>
#include <stdlib.h>

namespace AGS3 {
namespace AGS {
namespace Shared {
namespace SSTPaletteGen {

/* 5 bits per channel = 32K buckets.  uint16 counter (max 65535) is
 * enough — we cap per-bucket so a single image can't drown out others. */
static const int kBucketBits  = 5;
static const int kBucketCount = 1 << (kBucketBits * 3); /* 32768 */
static const int kPerSpriteCap = 200; /* sample at most N pixels per sprite */
static const uint16 kBucketSaturate = 0xFFFE;

static uint16 g_hist[kBucketCount];
static bool   g_active = false;
static uint32 g_total_samples = 0;
static uint32 g_unique_buckets = 0;

void Reset() {
	memset(g_hist, 0, sizeof(g_hist));
	g_total_samples = 0;
	g_unique_buckets = 0;
}

void SetActive(bool on) { g_active = on; }
bool IsActive()         { return g_active; }
int UniqueColoursSeen() { return (int)g_unique_buckets; }
uint32 TotalPixelsSampled() { return g_total_samples; }

static inline int rgb_bucket(uint8 r, uint8 g, uint8 b) {
	return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

static inline void bucket_to_rgb(int bucket, byte *r, byte *g, byte *b) {
	int rr = (bucket >> 10) & 0x1F;
	int gg = (bucket >>  5) & 0x1F;
	int bb = bucket        & 0x1F;
	/* 5-bit -> 8-bit via bit-replication (covers full 0..255 range). */
	*r = (byte)((rr << 3) | (rr >> 2));
	*g = (byte)((gg << 3) | (gg >> 2));
	*b = (byte)((bb << 3) | (bb >> 2));
}

void SamplePixels(const Bitmap *src) {
	if (!src) return;
	const int cd = src->GetColorDepth();
	if (cd <= 8) return; /* already quantized — useless */

	const int sw = src->GetWidth();
	const int sh = src->GetHeight();
	if (sw <= 0 || sh <= 0) return;

	const int total_pixels = sw * sh;
	const int step = (total_pixels > kPerSpriteCap) ? (total_pixels / kPerSpriteCap) : 1;

	int captured = 0;
	int linear = 0;
	int r, g, b, a;
	for (int y = 0; y < sh && captured < kPerSpriteCap; y++) {
		const uint8 *row = src->GetScanLine(y);
		for (int x = 0; x < sw && captured < kPerSpriteCap; x++) {
			if ((linear++ % step) != 0) continue;

			a = 255;
			if (cd == 15 || cd == 16) {
				uint16 v = ((const uint16 *)row)[x];
				r = getr16(v);
				g = getg16(v);
				b = getb16(v);
			} else if (cd >= 24) {
				uint32 v = ((const uint32 *)row)[x];
				r = getr32(v);
				g = getg32(v);
				b = getb32(v);
				if (cd == 32) a = geta32(v);
			} else {
				continue;
			}

			/* Skip transparent + magenta-mask pixels — they're noise. */
			if (a < 128) continue;
			if (r == 255 && g == 0 && b == 255) continue;

			int bk = rgb_bucket((uint8)r, (uint8)g, (uint8)b);
			if (g_hist[bk] == 0) g_unique_buckets++;
			if (g_hist[bk] < kBucketSaturate) g_hist[bk]++;
			g_total_samples++;
			captured++;
		}
	}
}

/* Median-cut palette selection.  Standard color quantization algorithm:
 * iteratively split the color space into N rectangular boxes, each
 * containing roughly equal "weight" (sum of pixel counts in the
 * histogram).  The output palette entry per box is the weighted mean
 * of its contained colors.
 *
 * Unlike top-N frequency selection, median-cut spreads palette slots
 * geometrically across color space — light-blue regions get slots even
 * if they're outnumbered by greens/browns in raw frequency count.
 * That fixes both the night-scene-too-dark (under-represented dark
 * range) and the menu-rendered-green (under-represented light blue)
 * symptoms.
 *
 * Layout of out_rgb (768 bytes):
 *   slot 0           : pure black (transparent mask)
 *   slots 1..32      : reserved dark-gradient ramp (Path A — covers
 *                      cool/warm/neutral/saturated darks; median-cut
 *                      consistently under-allocated this region on HQ
 *                      because dim night-village samples are vastly
 *                      outnumbered by 102 daytime room bgs)
 *   slots 33..239    : median-cut output (207 boxes)
 *   slots 240..255   : 16-entry grayscale ramp (snow/skin/highlights) */

/* 32-entry low-luminance reserve: cool / warm / neutral / saturated
 * darks roughly in the (0..96)^3 corner of RGB space.  Acts as a
 * "safety net" for sprite quantize bestfit when the active scene has
 * deep shadows the median-cut allocation skipped. */
static const byte kDarkRamp[32 * 3] = {
	/* 8 cool / navy (B-leaning) */
	  4,   4,  16,    8,   8,  32,   12,  12,  48,   16,  16,  64,
	  8,  12,  24,   16,  20,  44,   24,  32,  68,   32,  44,  92,
	/* 8 warm / brown (R-leaning, with G for wood/skin shadow) */
	 16,   8,   4,   32,  16,   8,   48,  24,  12,   64,  32,  16,
	 24,  16,   8,   44,  28,  12,   60,  40,  20,   80,  56,  28,
	/* 8 neutral grays from very dark to dim */
	  8,   8,   8,   16,  16,  16,   24,  24,  28,   32,  32,  40,
	 44,  44,  48,   56,  56,  60,   72,  72,  76,   88,  88,  92,
	/* 8 deep saturated darks (purples, dark greens, deep blues) */
	 20,   8,  32,   40,  16,  64,   12,  28,  16,   24,  52,  28,
	 36,  20,  56,   52,  28,  80,   16,  36,  44,   28,  60,  72,
};

struct Bucket { uint16 r, g, b; uint16 count; };
struct Box {
	int first, last; /* indices into the bucket array */
	uint8 rmin, rmax, gmin, gmax, bmin, bmax;
	uint32 weight;   /* sum of contained bucket counts */
};

static int cmp_r(const void *a, const void *b) {
	return ((const Bucket *)a)->r - ((const Bucket *)b)->r;
}
static int cmp_g(const void *a, const void *b) {
	return ((const Bucket *)a)->g - ((const Bucket *)b)->g;
}
static int cmp_b(const void *a, const void *b) {
	return ((const Bucket *)a)->b - ((const Bucket *)b)->b;
}

static void box_recompute(Box &bx, Bucket *bks) {
	bx.rmin = bx.gmin = bx.bmin = 31;
	bx.rmax = bx.gmax = bx.bmax = 0;
	bx.weight = 0;
	for (int i = bx.first; i <= bx.last; i++) {
		const Bucket &bk = bks[i];
		if (bk.r < bx.rmin) bx.rmin = (uint8)bk.r;
		if (bk.r > bx.rmax) bx.rmax = (uint8)bk.r;
		if (bk.g < bx.gmin) bx.gmin = (uint8)bk.g;
		if (bk.g > bx.gmax) bx.gmax = (uint8)bk.g;
		if (bk.b < bx.bmin) bx.bmin = (uint8)bk.b;
		if (bk.b > bx.bmax) bx.bmax = (uint8)bk.b;
		bx.weight += bk.count;
	}
}

/* Split a box: choose the longest axis, sort buckets along it, split
 * at the median by weight. */
static bool box_split(Box &src, Bucket *bks, Box &dst) {
	int rrange = src.rmax - src.rmin;
	int grange = src.gmax - src.gmin;
	int brange = src.bmax - src.bmin;
	if (rrange == 0 && grange == 0 && brange == 0) return false;
	if (src.first >= src.last) return false;

	int (*cmp)(const void *, const void *);
	if (rrange >= grange && rrange >= brange) cmp = cmp_r;
	else if (grange >= brange) cmp = cmp_g;
	else cmp = cmp_b;

	qsort(bks + src.first, src.last - src.first + 1, sizeof(Bucket), cmp);

	uint32 half = src.weight / 2;
	uint32 cum = 0;
	int split = src.first;
	for (int i = src.first; i <= src.last; i++) {
		cum += bks[i].count;
		if (cum >= half) { split = i; break; }
	}
	if (split <= src.first) split = src.first + 1;
	if (split > src.last) split = src.last;

	dst.first = split;
	dst.last  = src.last;
	src.last  = split - 1;
	box_recompute(src, bks);
	box_recompute(dst, bks);
	return true;
}

void Generate(byte *out_rgb) {
	memset(out_rgb, 0, 768);

	/* Slot 0: black. */
	out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;

	/* Slots 1..32: reserved dark-gradient ramp. */
	const int kDarkStart = 1;
	const int kDarkCount = 32;
	memcpy(out_rgb + kDarkStart * 3, kDarkRamp, sizeof(kDarkRamp));

	/* Slots 240..255: 16-entry grayscale ramp 0..255. */
	const int kGrayStart = 240;
	const int kGrayCount = 16;
	for (int i = 0; i < kGrayCount; i++) {
		byte v = (byte)(i * 255 / (kGrayCount - 1));
		int idx = kGrayStart + i;
		out_rgb[idx*3 + 0] = v;
		out_rgb[idx*3 + 1] = v;
		out_rgb[idx*3 + 2] = v;
	}

	/* Build the bucket list from the histogram.  Each bucket carries
	 * its 5-bit centroid (0..31 per channel) and weight (sample count). */
	static Bucket g_bks[kBucketCount];
	int n_bks = 0;
	for (int bk = 0; bk < kBucketCount; bk++) {
		if (g_hist[bk] == 0) continue;
		g_bks[n_bks].r = (uint16)((bk >> 10) & 0x1F);
		g_bks[n_bks].g = (uint16)((bk >>  5) & 0x1F);
		g_bks[n_bks].b = (uint16)( bk        & 0x1F);
		g_bks[n_bks].count = g_hist[bk];
		n_bks++;
	}

	/* Median-cut targets slots kMedianStart..kGrayStart-1 (207 slots). */
	const int kMedianStart = kDarkStart + kDarkCount; /* 33 */
	const int kTargetBoxes = kGrayStart - kMedianStart; /* 207 */
	if (n_bks == 0) {
		/* No samples — fall back to a 4×4×4 cube for the median range. */
		for (int slot = kMedianStart; slot < kGrayStart; slot++) {
			int fb = slot - kMedianStart;
			int rr = (fb >> 4) & 0x3;
			int gg = (fb >> 2) & 0x3;
			int bb = fb        & 0x3;
			out_rgb[slot*3 + 0] = (byte)(rr * 85);
			out_rgb[slot*3 + 1] = (byte)(gg * 85);
			out_rgb[slot*3 + 2] = (byte)(bb * 85);
		}
		return;
	}

	/* Median-cut: start with one box covering all buckets, split until
	 * we have kTargetBoxes (or run out of splittable boxes). */
	static Box boxes[kTargetBoxes + 1];
	int n_boxes = 1;
	boxes[0].first = 0;
	boxes[0].last  = n_bks - 1;
	box_recompute(boxes[0], g_bks);

	while (n_boxes < kTargetBoxes) {
		/* Find the box with the largest weight × axis-range to split. */
		int best = -1;
		uint32 best_score = 0;
		for (int i = 0; i < n_boxes; i++) {
			int rr = boxes[i].rmax - boxes[i].rmin;
			int gr = boxes[i].gmax - boxes[i].gmin;
			int br = boxes[i].bmax - boxes[i].bmin;
			int range = (rr > gr) ? (rr > br ? rr : br) : (gr > br ? gr : br);
			if (range == 0) continue;
			if (boxes[i].first >= boxes[i].last) continue;
			uint32 score = (uint32)range * boxes[i].weight;
			if (score > best_score) { best_score = score; best = i; }
		}
		if (best < 0) break; /* all boxes are atomic */

		if (!box_split(boxes[best], g_bks, boxes[n_boxes])) break;
		n_boxes++;
	}

	/* Emit each box's weighted-average colour into median-cut slots. */
	for (int i = 0; i < n_boxes; i++) {
		uint32 sumR = 0, sumG = 0, sumB = 0, sumW = 0;
		for (int j = boxes[i].first; j <= boxes[i].last; j++) {
			sumR += (uint32)g_bks[j].r * g_bks[j].count;
			sumG += (uint32)g_bks[j].g * g_bks[j].count;
			sumB += (uint32)g_bks[j].b * g_bks[j].count;
			sumW += g_bks[j].count;
		}
		if (sumW == 0) sumW = 1;
		/* Centroid in 5-bit space → expand to 8-bit. */
		int rr = (int)(sumR / sumW);
		int gg = (int)(sumG / sumW);
		int bb = (int)(sumB / sumW);
		int slot = kMedianStart + i;
		out_rgb[slot*3 + 0] = (byte)((rr << 3) | (rr >> 2));
		out_rgb[slot*3 + 1] = (byte)((gg << 3) | (gg >> 2));
		out_rgb[slot*3 + 2] = (byte)((bb << 3) | (bb >> 2));
	}

	/* Pad any unused median slots (n_boxes < kTargetBoxes) with cube. */
	for (int slot = kMedianStart + n_boxes; slot < kGrayStart; slot++) {
		int fb = slot - kMedianStart - n_boxes;
		int rr = (fb >> 4) & 0x3;
		int gg = (fb >> 2) & 0x3;
		int bb = fb        & 0x3;
		out_rgb[slot*3 + 0] = (byte)(rr * 85);
		out_rgb[slot*3 + 1] = (byte)(gg * 85);
		out_rgb[slot*3 + 2] = (byte)(bb * 85);
	}
}

void TranslateRoomBgPalette(Bitmap *bg, const ::AGS3::color *src_palette) {
	if (!bg || !src_palette) return;
	if (bg->GetColorDepth() != 8) return; /* only 8-bit bgs need translation */

	const int w = bg->GetWidth();
	const int h = bg->GetHeight();
	if (w <= 0 || h <= 0) return;

	/* Per-bestfit cache: idx → translated_idx.  256 entries; -1 = unknown.
	 * Avoids running bestfit_color w×h times when the bg only uses a
	 * handful of distinct palette indices. */
	int xlat[256];
	for (int i = 0; i < 256; i++) xlat[i] = -1;

	for (int y = 0; y < h; y++) {
		uint8 *row = bg->GetScanLineForWriting(y);
		for (int x = 0; x < w; x++) {
			uint8 src_idx = row[x];
			int new_idx = xlat[src_idx];
			if (new_idx < 0) {
				/* AGS palettes are 6-bit per channel (0..63).  Both
				 * src_palette and _G(current_palette) live in that
				 * same range so we can compare directly. */
				int r = src_palette[src_idx].r;
				int g = src_palette[src_idx].g;
				int b = src_palette[src_idx].b;
				new_idx = bestfit_color(_G(current_palette), r, g, b);
				xlat[src_idx] = new_idx;
			}
			row[x] = (uint8)new_idx;
		}
	}
}

int RoomBackgroundSlotCount() { return MAX_ROOMS; }

bool SampleRoomBackground(int room_id) {
	if (room_id < 0 || room_id >= MAX_ROOMS) return false;
	String filename;
	filename.Format("room%d.crm", room_id);
	if (!_GP(AssetMgr)->DoesAssetExist(filename)) {
		if (room_id == 0 && _GP(AssetMgr)->DoesAssetExist("intro.crm"))
			filename = "intro.crm";
		else
			return false;
	}
	RoomStruct room;
	load_room(filename, &room, _GP(game).IsLegacyHiRes(), _GP(game).SpriteInfos);
	for (size_t i = 0; i < room.BgFrameCount; ++i) {
		Bitmap *bg = room.BgFrames[i].Graphic.get();
		if (bg) SamplePixels(bg);
	}
	return true;
}

int SampleAllRoomBackgrounds() {
	int sampled = 0;
	for (int i = 0; i < RoomBackgroundSlotCount(); ++i)
		if (SampleRoomBackground(i)) ++sampled;
	return sampled;
}

} // namespace SSTPaletteGen
} // namespace Shared
} // namespace AGS
} // namespace AGS3
