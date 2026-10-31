/* SST AGS asset disk cache.  See sst-ags-asset-cache.h for format. */

#define FORBIDDEN_SYMBOL_EXCEPTION_fflush
#define FORBIDDEN_SYMBOL_EXCEPTION_unistd_h
#define FORBIDDEN_SYMBOL_EXCEPTION_FILE
#define FORBIDDEN_SYMBOL_EXCEPTION_fopen
#define FORBIDDEN_SYMBOL_EXCEPTION_fclose
#define FORBIDDEN_SYMBOL_EXCEPTION_fread
#define FORBIDDEN_SYMBOL_EXCEPTION_fwrite
#define FORBIDDEN_SYMBOL_EXCEPTION_FILE

#include "ags/shared/ac/sst_asset_cache.h"
#include "ags/shared/gfx/allegro_bitmap.h"
#include "ags/shared/util/directory.h"
#include "ags/shared/util/path.h"
#include "ags/shared/util/stdio_compat.h"
#include "ags/shared/util/string.h"
#include "ags/shared/debugging/out.h"

#include "common/config-manager.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/md5.h"
#include "common/memstream.h"
#include "common/hashmap.h"
#include "common/algorithm.h"
#include "ags/globals.h"
#include "ags/shared/core/asset_manager.h"
#include "ags/lib/allegro/color.h"
#include "common/textconsole.h"
#include "ags/engine/sst_precompute_dialog.h"
#include "ags/engine/media/audio/sst_pcm_cache.h"

#include <stdio.h>
#include <errno.h>
#include <unistd.h>

namespace AGS3 {
namespace AGS {
namespace Shared {
namespace SSTAssetCache {

static const uint32 kMagic    = 0x51415353u;     // 'S','S','A','Q'
static const uint32 kVersion  = 1u;
static const char  *kDirName  = "_quantized";
static const char  *kPaletteName = "_palette.bin";
static const uint32 kPaletteMagic   = 0x544c5053u; // 'S','P','L','T'
static const uint32 kPaletteVersion = 1u;

static bool s_palette_conversion_required = false;
static bool   s_enabled         = false;
static bool   s_store_suppressed = false;
static String s_cache_dir;     // <game-dir>/_quantized
static bool s_lookup_attempted = false, s_lookup_valid = false;
static String s_game_dir;      // <game-dir>

/* Compute the per-sprite filename under the cache dir. */
static String SpritePath(int32 index) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%06d.pal8", (int)index);
	return Path::ConcatPaths(s_cache_dir, String(buf));
}

static String TemporaryPath(const String &target) {
	String path;
	path.Format("%s.tmp-%lu", target.GetCStr(), (unsigned long)getpid());
	return path;
}

static String PalettePath() {
	return Path::ConcatPaths(s_game_dir, String(kPaletteName));
}

struct GraphicsHashProgress {
	AGS::Engine::SSTPreparationScreen &screen;
	Common::String name;
	uint64 bytes, total, last;
	GraphicsHashProgress(AGS::Engine::SSTPreparationScreen &s, const Common::String &n, uint64 length)
		: screen(s), name(n), bytes(0), total(length), last(0) {}
	static bool tick(void *opaque, int count) {
		GraphicsHashProgress *p = (GraphicsHashProgress *)opaque;
		p->bytes += count;
		if (p->bytes - p->last < 65536) return true;
		p->last = p->bytes;
		return p->screen.update("Checking original graphics", p->bytes / 1024, (p->total + 1023) / 1024, p->name.c_str());
	}
};

void SetPaletteConversionRequired(bool required) {
	s_palette_conversion_required = required;
	/* Do not borrow another game's palette or sprites during startup. */
	s_enabled = false;
	s_lookup_attempted = s_lookup_valid = false;
}

bool RequiresPaletteConversion() { return s_palette_conversion_required; }

bool Init() {
	s_enabled = false;
	s_store_suppressed = false;
	s_lookup_attempted = s_lookup_valid = false;
	if (g_system->getScreenFormat().bytesPerPixel != 1) return false;
	Common::Path gameDir = ConfMan.getPath("path");
	if (gameDir.empty()) return false;
	AGS::Engine::SSTPreparationScreen screen;
	if (!screen.update("Checking original graphics", 0, 1, "", true)) return false;
	Common::FSList files;
	Common::FSNode(gameDir).getChildren(files, Common::FSNode::kListFilesOnly);
	Common::sort(files.begin(), files.end(), [](const Common::FSNode &a, const Common::FSNode &b) { return a.getName() < b.getName(); });
	Common::Array<Common::Path> sources;
	Common::HashMap<Common::String, bool> seen;
	for (size_t i = 0; i < _GP(AssetMgr)->GetLibraryCount(); ++i) {
		const AssetLibInfo *lib = _GP(AssetMgr)->GetLibraryInfo(i);
		Common::String lower(lib->BaseFileName.GetCStr()); lower.toLowercase();
		if (lower.hasSuffix(".vox") || Common::FSNode(Common::Path(lib->BasePath.GetCStr())).isDirectory()) continue;
		for (size_t part = 0; part < lib->LibFileNames.size(); ++part) {
			Common::Path path = Common::Path(lib->BaseDir.GetCStr()).join(lib->LibFileNames[part].GetCStr());
			/* AGS paths are virtual and case-insensitive. Resolve archive
			 * basenames to actual filesystem spelling before direct IO. */
			if (!Common::FSNode(path).exists()) {
				Common::String partName = Common::FSNode(Common::Path(lib->LibFileNames[part].GetCStr())).getName();
				for (uint n = 0; n < files.size(); ++n)
					if (files[n].getName().equalsIgnoreCase(partName)) { path = files[n].getPath(); break; }
			}
			Common::String key = path.toString('/');
			if (!seen.contains(key)) { seen[key] = true; sources.push_back(path); }
		}
	}
	/* Loose room/sprite files can override packed assets. Include their
	 * contents in the cache identity as well as all main-library parts. */

	for (uint i = 0; i < files.size(); ++i) {
		Common::String lower = files[i].getName(); lower.toLowercase();
		if (!lower.hasSuffix(".crm") && !lower.hasSuffix(".spr") && !lower.hasSuffix(".dat") && !lower.hasSuffix(".ags")) continue;
		Common::String key = files[i].getPath().toString('/');
		if (!seen.contains(key)) { seen[key] = true; sources.push_back(files[i].getPath()); }
	}
	if (sources.empty()) error("No original AGS graphics data found for cache identity");
	AGS::Engine::SSTSourceDigestCache digests;
	Common::String identity("graphics-v2:");
	for (uint i = 0; i < sources.size(); ++i) {
		Common::String name = Common::FSNode(sources[i]).getName();
		Common::String stamp = AGS::Engine::SSTFileMetadataKey(sources[i]);
		Common::String key = stamp.empty() ? Common::String() : "file-" + stamp;
		Common::String hash;
		if (!digests.read(key, hash) || hash.size() != 32) {
			Common::File input;
			if (!input.open(Common::FSNode(sources[i]))) error("Cannot read original graphics source: %s", sources[i].toString('/').c_str());
			GraphicsHashProgress progress(screen, name, input.size());
			if (!screen.update("Checking original graphics", 0, (input.size() + 1023) / 1024, name.c_str(), true)) return false;
			hash = Common::computeStreamMD5AsString(input, 0, GraphicsHashProgress::tick, &progress);
			if (hash.empty()) return false;
			if (input.err()) error("Read error checking original graphics");
			digests.write(key, hash);
		}
		identity += name + ":" + hash + ";";
	}
	Common::MemoryReadStream identityStream((const byte *)identity.c_str(), identity.size());
	Common::String fingerprint = Common::computeStreamMD5AsString(identityStream);
	Common::FSNode parent = Common::FSNode(AGS::Engine::SSTCacheRoot()).getChild("graphics-v2-" + fingerprint);
	if ((!parent.exists() && !parent.createDirectory()) || !parent.isDirectory()) error("Cannot create graphics cache directory");
	s_game_dir = parent.getPath().toString('/').c_str();
	s_cache_dir = Path::ConcatPaths(s_game_dir, String(kDirName));
	if (!Directory::CreateDirectory(s_cache_dir) && !ags_directory_exists(s_cache_dir.GetCStr())) error("Cannot create indexed sprite cache");
	s_enabled = true;
	Debug::Printf(kDbgMsg_Info, "SSTAssetCache: enabled at %s", s_cache_dir.GetCStr());
	return true;
}

const byte *GetColourLookup(const byte *palette6) {

 static byte expected[768];
 static byte lookup[64 * 64 * 64];
 if (!s_enabled) return nullptr;
 if (!s_lookup_attempted) {
  s_lookup_attempted = true;
  String path = Path::ConcatPaths(s_game_dir, String("_palette-nearest.bin"));
  FILE *fp = fopen(path.GetCStr(), "rb");
  if (fp) {
   char magic[8];
   s_lookup_valid = fread(magic, 1, 8, fp) == 8 && memcmp(magic, "SSNMAP1", 8) == 0 &&
           fread(expected, 1, sizeof(expected), fp) == sizeof(expected) &&
           fread(lookup, 1, sizeof(lookup), fp) == sizeof(lookup);
   fclose(fp);
   if (s_lookup_valid) Debug::Printf(kDbgMsg_Info, "SSTAssetCache: loaded exact palette colour lookup");
  }
 }
 return s_lookup_valid && memcmp(expected, palette6, sizeof(expected)) == 0 ? lookup : nullptr;
}

bool IsEnabled() {
	return s_enabled;
}

bool HasEntry(int32 index) {
	if (!s_enabled) return false;
	Common::File file;
	Common::FSNode node(Common::Path(SpritePath(index).GetCStr()));
	if (!node.exists() || !file.open(node)) return false;
	uint32 h[6];
	if (file.read(h, sizeof(h)) != sizeof(h)) return false;
	return h[0] == kMagic && h[1] == kVersion && h[2] > 0 && h[2] <= 32768 &&
		h[3] > 0 && h[3] <= 32768 && (uint64)h[2] * h[3] == h[5] &&
		file.size() == (int64)sizeof(h) + h[5];
}

void InvalidateEntry(int32 index) {
	if (s_enabled) remove(SpritePath(index).GetCStr());
}

Bitmap *TryLoad(int32 index, uint32 *sprite_flags_out) {
	/* Palette sampling must see the original high-colour pixels, never
	 * an indexed entry quantized against an earlier palette. */
	if (s_store_suppressed) return nullptr;
	if (!s_enabled) return nullptr;

	String p = SpritePath(index);
	FILE *fp = fopen(p.GetCStr(), "rb");
	if (!fp) return nullptr;

	uint32 hdr[6];
	if (fread(hdr, sizeof(hdr), 1, fp) != 1) {
		fclose(fp);
		Debug::Printf(kDbgMsg_Warn,
			"SSTAssetCache: short header for sprite %d", (int)index);
		return nullptr;
	}
	if (hdr[0] != kMagic || hdr[1] != kVersion) {
		fclose(fp);
		Debug::Printf(kDbgMsg_Warn,
			"SSTAssetCache: bad magic/version for sprite %d", (int)index);
		return nullptr;
	}

	uint32 width      = hdr[2];
	uint32 height     = hdr[3];
	uint32 flags      = hdr[4];
	uint32 pixel_size = hdr[5];

	if (width == 0 || height == 0 || width > 32768 || height > 32768
	    || pixel_size != width * height) {
		fclose(fp);
		Debug::Printf(kDbgMsg_Warn,
			"SSTAssetCache: nonsense dimensions for sprite %d (%ux%u, %u bytes)",
			(int)index, width, height, pixel_size);
		return nullptr;
	}

	Bitmap *bmp = new Bitmap();
	if (!bmp->Create((int)width, (int)height, 8)) {
		delete bmp;
		fclose(fp);
		Debug::Printf(kDbgMsg_Warn,
			"SSTAssetCache: Bitmap::Create failed for sprite %d", (int)index);
		return nullptr;
	}

	/* Pixel rows.  AGS bitmaps are linear; pitch == width for 8bpp.
	 * Read row-by-row in case of any future pitch != width quirk. */
	for (uint32 y = 0; y < height; y++) {
		uint8 *row = bmp->GetScanLineForWriting((int)y);
		if (fread(row, 1, width, fp) != width) {
			delete bmp;
			fclose(fp);
			Debug::Printf(kDbgMsg_Warn,
				"SSTAssetCache: short row %u for sprite %d", y, (int)index);
			return nullptr;
		}
	}
	fclose(fp);

	if (sprite_flags_out)
		*sprite_flags_out = flags;
	return bmp;
}

void SetStoreSuppressed(bool on) { s_store_suppressed = on; }

bool HasPaletteFile() {
	byte rgb[768];
	return LoadPalette(rgb);
}

bool SavePalette(const byte *rgb768) {
	if (!s_enabled || !rgb768) return false;
	String target = PalettePath();
	String p = TemporaryPath(target);
	FILE *fp = fopen(p.GetCStr(), "wb");
	if (!fp) {
		Debug::Printf(kDbgMsg_Warn,
			"SSTAssetCache: cannot open %s for write (errno=%d)", p.GetCStr(), errno);
		return false;
	}
	uint32 hdr[2] = { kPaletteMagic, kPaletteVersion };
	if (fwrite(hdr, sizeof(hdr), 1, fp) != 1) {
		fclose(fp);
		remove(p.GetCStr());
		return false;
	}
	if (fwrite(rgb768, 768, 1, fp) != 1) {
		fclose(fp);
		remove(p.GetCStr());
		return false;
	}
	bool published = fflush(fp) == 0 && fsync(fileno(fp)) == 0;
	if (fclose(fp) != 0) published = false;
	if (!published || rename(p.GetCStr(), target.GetCStr()) != 0) {
		remove(p.GetCStr()); return false;
	}
	Debug::Printf(kDbgMsg_Info,
		"SSTAssetCache: wrote game-tuned palette to %s", target.GetCStr());
	return true;
}

bool LoadPalette(byte *rgb768) {
	if (!s_enabled || !rgb768) return false;
	String p = PalettePath();
	FILE *fp = fopen(p.GetCStr(), "rb");
	if (!fp) return false;
	uint32 hdr[2] = { 0, 0 };
	if (fread(hdr, sizeof(hdr), 1, fp) != 1) { fclose(fp); return false; }
	if (hdr[0] != kPaletteMagic || hdr[1] != kPaletteVersion) {
		fclose(fp);
		Debug::Printf(kDbgMsg_Warn,
			"SSTAssetCache: %s magic/version mismatch", p.GetCStr());
		return false;
	}
	if (fread(rgb768, 768, 1, fp) != 1) { fclose(fp); return false; }
	fclose(fp);
	return true;
}

void Store(int32 index, const Bitmap *image, uint32 sprite_flags) {
	if (!s_enabled || !image) return;
	if (s_store_suppressed) return;
	if (image->GetColorDepth() != 8) {
		/* We only cache the 8bpp post-quantize result.  If the engine
		 * somehow handed us a non-8bpp bitmap here it's a bug elsewhere;
		 * log and skip rather than write garbage. */
		Debug::Printf(kDbgMsg_Warn,
			"SSTAssetCache: refusing to store sprite %d at depth %d",
			(int)index, image->GetColorDepth());
		return;
	}

	String target = SpritePath(index);
	String p = TemporaryPath(target);
	FILE *fp = fopen(p.GetCStr(), "wb");
	if (!fp) {
		Debug::Printf(kDbgMsg_Warn,
			"SSTAssetCache: cannot open %s for write (errno=%d)", p.GetCStr(), errno);
		return;
	}

	uint32 width      = (uint32)image->GetWidth();
	uint32 height     = (uint32)image->GetHeight();
	uint32 pixel_size = width * height;

	uint32 hdr[6] = { kMagic, kVersion, width, height, sprite_flags, pixel_size };
	if (fwrite(hdr, sizeof(hdr), 1, fp) != 1) {
		fclose(fp);
		remove(p.GetCStr()); /* don't leave a truncated file the cache will read back */
		Debug::Printf(kDbgMsg_Warn,
			"SSTAssetCache: header write failed for sprite %d", (int)index);
		return;
	}

	for (uint32 y = 0; y < height; y++) {
		const uint8 *row = image->GetScanLine((int)y);
		if (fwrite(row, 1, width, fp) != width) {
			fclose(fp);
			remove(p.GetCStr());
			Debug::Printf(kDbgMsg_Warn,
				"SSTAssetCache: row %u write failed for sprite %d", y, (int)index);
			return;
		}
	}
	bool published = fflush(fp) == 0 && fsync(fileno(fp)) == 0;
	if (fclose(fp) != 0) published = false;
	if (!published || rename(p.GetCStr(), target.GetCStr()) != 0) {
		remove(p.GetCStr());
		Debug::Printf(kDbgMsg_Warn, "SSTAssetCache: failed to publish sprite %d", (int)index);
	}
}

bool EnsureColourLookup() {
	if (!s_enabled) return false;
	byte palette[768];
	for (int i = 0; i < 256; ++i) {
		palette[i*3] = _G(current_palette)[i].r;
		palette[i*3+1] = _G(current_palette)[i].g;
		palette[i*3+2] = _G(current_palette)[i].b;
	}
	if (GetColourLookup(palette)) return true;
	AGS::Engine::SSTPreparationScreen screen;
	Common::Array<byte> lookup;
	lookup.resize(64 * 64 * 64);
	for (int r = 0; r < 64; ++r) {
		if (!screen.update("Preparing colour lookup", r, 64, "", true)) return false;
		for (int g = 0; g < 64; ++g)
			for (int b = 0; b < 64; ++b)
				lookup[(r << 12) | (g << 6) | b] = bestfit_color(_G(current_palette), r, g, b);
	}
	String target = Path::ConcatPaths(s_game_dir, String("_palette-nearest.bin"));
	String temp = TemporaryPath(target);
	FILE *file = fopen(temp.GetCStr(), "wb");
	if (!file) return false;
	bool ok = fwrite("SSNMAP1", 1, 8, file) == 8 &&
		fwrite(palette, 1, sizeof(palette), file) == sizeof(palette) &&
		fwrite(&lookup[0], 1, lookup.size(), file) == lookup.size();
	if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
	if (fclose(file) != 0) ok = false;
	if (ok) ok = rename(temp.GetCStr(), target.GetCStr()) == 0;
	if (!ok) { remove(temp.GetCStr()); return false; }
	s_lookup_attempted = s_lookup_valid = false;
	screen.update("Colour lookup ready", 64, 64, "", true);
	return GetColourLookup(palette) != nullptr;
}

} // namespace SSTAssetCache
} // namespace Shared
} // namespace AGS
} // namespace AGS3
