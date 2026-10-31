/* Native Solaris first-run PCM cache. Original AGS libraries stay read-only. */
#include <sys/stat.h>
#define FORBIDDEN_SYMBOL_EXCEPTION_getenv
#define FORBIDDEN_SYMBOL_EXCEPTION_unistd_h
#define FORBIDDEN_SYMBOL_EXCEPTION_FILE
#define FORBIDDEN_SYMBOL_EXCEPTION_fopen
#define FORBIDDEN_SYMBOL_EXCEPTION_fclose
#define FORBIDDEN_SYMBOL_EXCEPTION_fwrite
#define FORBIDDEN_SYMBOL_EXCEPTION_fseek
#define FORBIDDEN_SYMBOL_EXCEPTION_fflush
#define FORBIDDEN_SYMBOL_EXCEPTION_ferror
#include "ags/engine/media/audio/sst_pcm_cache.h"
#include "ags/engine/sst_precompute_dialog.h"
#include "ags/globals.h"
#include "ags/shared/util/stream.h"
#include "audio/audiostream.h"
#include "common/events.h"
#include "audio/decoders/mp3.h"
#include "audio/decoders/vorbis.h"
#include "audio/decoders/wave.h"
#include "common/bufferedstream.h"
#include "common/config-manager.h"
#include "common/endian.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/hashmap.h"
#include "common/algorithm.h"
#include "common/md5.h"
#include "common/memstream.h"
#include "common/system.h"
#include "common/textconsole.h"
#include <stdio.h>
#include <unistd.h>

namespace AGS3 { namespace AGS { namespace Engine {
static Common::HashMap<Common::String, Common::Path> s_pcmFiles;

static Common::String assetKey(const Shared::AssetPath &asset) {
	Common::String key = Common::String::format("%s:%s", asset.Filter.GetCStr(), asset.Name.GetCStr());
	key.toLowercase();
	return key;
}

Common::SeekableReadStream *OpenSSTCachedAudio(const Shared::AssetPath &asset) {
	Common::String key = assetKey(asset);
	if (!s_pcmFiles.contains(key)) return nullptr;
	Common::File *file = new Common::File();
	if (!file->open(Common::FSNode(s_pcmFiles[key]))) { delete file; return nullptr; }
	static int logged = 0;
	if (logged++ < 12) warning("[sst-pcm-cache] playback %s", key.c_str());
	return Common::wrapBufferedSeekableReadStream(file, 65536, DisposeAfterUse::YES);
}

static bool validPCM(const Common::Path &path) {
	Common::File file;
	Common::FSNode node(path);
	if (!node.exists() || !file.open(node)) return false;
	byte h[44];
	if (file.read(h, sizeof(h)) != sizeof(h)) return false;
	uint32 size = READ_LE_UINT32(h + 40);
	uint16 channels = READ_LE_UINT16(h + 22);
	uint32 rate = READ_LE_UINT32(h + 24);
	return !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) &&
		READ_LE_UINT32(h + 16) == 16 && READ_LE_UINT16(h + 20) == 1 &&
		(channels == 1 || channels == 2) && rate > 0 && rate <= 192000 &&
		READ_LE_UINT16(h + 34) == 16 && !memcmp(h + 36, "data", 4) &&
		READ_LE_UINT16(h + 32) == channels * 2 &&
		READ_LE_UINT32(h + 28) == rate * channels * 2 &&
		size > 0 && size % (channels * 2) == 0 && size <= 0xffffffd0U &&
		READ_LE_UINT32(h + 4) == size + 36 && file.size() == (int64)size + 44;
}

struct HashProgress {
	SSTPreparationScreen &screen;
	Common::String name;
	int index, total;
	uint64 bytes, lastUpdate;
	HashProgress(SSTPreparationScreen &s, const char *n, int i, int t)
		: screen(s), name(n), index(i), total(t), bytes(0), lastUpdate(0) {}
	static bool tick(void *opaque, int count) {
		HashProgress *p = (HashProgress *)opaque;
		p->bytes += count;
		if (p->bytes - p->lastUpdate < 65536) return true;
		p->lastUpdate = p->bytes;
		Common::String detail = Common::String::format("%s (%llu KB)", p->name.c_str(), (unsigned long long)(p->bytes / 1024));
		return p->screen.update("Checking original audio", p->index, p->total, detail.c_str());
	}
};

static bool writePCM(Audio::AudioStream &audio, const Common::Path &path,
		SSTPreparationScreen &screen, const char *name, int index, int total) {
	Common::String target = path.toString(Common::Path::kNativeSeparator);
	Common::String temp = Common::String::format("%s.tmp-%lu", target.c_str(), (unsigned long)getpid());
	FILE *file = fopen(temp.c_str(), "wb");
	if (!file) return false;
	byte header[44] = {0};
	bool ok = fwrite(header, 1, 44, file) == 44;
	uint32 bytes = 0;
	int16 samples[4096];
	byte encoded[8192];
	while (ok && !audio.endOfData()) {
		int n = audio.readBuffer(samples, ARRAYSIZE(samples));
		if (n <= 0) { ok = audio.endOfData(); break; }
		if ((uint64)bytes + n * 2 > 0xffffffd0U) { ok = false; break; }
		for (int i = 0; i < n; ++i) WRITE_LE_UINT16(encoded + i * 2, (uint16)samples[i]);
		ok = fwrite(encoded, 2, n, file) == (size_t)n;
		bytes += n * 2;
		Common::String detail = Common::String::format("%s (%u KB)", name, bytes / 1024);
		if (!screen.update("Decoding audio to PCM", index, total, detail.c_str())) { ok = false; break; }
	}
	uint16 channels = audio.isStereo() ? 2 : 1;
	uint32 rate = audio.getRate();
	ok = ok && bytes > 0 && bytes % (channels * 2) == 0 && rate > 0 && rate <= 192000;
	memcpy(header, "RIFF", 4); WRITE_LE_UINT32(header + 4, bytes + 36);
	memcpy(header + 8, "WAVEfmt ", 8); WRITE_LE_UINT32(header + 16, 16);
	WRITE_LE_UINT16(header + 20, 1); WRITE_LE_UINT16(header + 22, channels);
	WRITE_LE_UINT32(header + 24, rate); WRITE_LE_UINT32(header + 28, rate * channels * 2);
	WRITE_LE_UINT16(header + 32, channels * 2); WRITE_LE_UINT16(header + 34, 16);
	memcpy(header + 36, "data", 4); WRITE_LE_UINT32(header + 40, bytes);
	if (ok) ok = fseek(file, 0, SEEK_SET) == 0 && fwrite(header, 1, 44, file) == 44 && fflush(file) == 0 && !ferror(file) && fsync(fileno(file)) == 0;
	if (fclose(file) != 0) ok = false;
	if (ok) ok = rename(temp.c_str(), target.c_str()) == 0;
	if (!ok) remove(temp.c_str());
	return ok && validPCM(path);
}

static bool ensureCacheDirectory(const Common::FSNode &dir) {
	if (dir.exists()) return dir.isDirectory();
	Common::FSNode parent = dir.getParent();
	if (parent.getPath() == dir.getPath()) return false;
	return ensureCacheDirectory(parent) && dir.createDirectory();
}

Common::Path SSTCacheRoot() {
	Common::Path path;
	if (ConfMan.hasKey("sst_cache_path")) path = ConfMan.getPath("sst_cache_path");
	else {
		const char *xdg = getenv("XDG_CACHE_HOME");
		const char *home = getenv("HOME");
		if (xdg && xdg[0] == '/') path = Common::Path(xdg).join("scummvm/ags");
		else if (home && home[0] == '/') path = Common::Path(home).join(".cache/scummvm/ags");
	}
	Common::String native = path.toString(Common::Path::kNativeSeparator);
	if (native.empty() || native[0] != '/' || !ensureCacheDirectory(Common::FSNode(path)))
		error("Cannot create ScummVM asset cache; check HOME, XDG_CACHE_HOME or sst_cache_path");
	return path;
}

static Common::String digestText(const Common::String &text) {
	Common::MemoryReadStream stream((const byte *)text.c_str(), text.size());
	return Common::computeStreamMD5AsString(stream);
}

Common::String SSTFileMetadataKey(const Common::Path &path) {
	Common::String native = path.toString(Common::Path::kNativeSeparator);
	struct stat info;
	if (stat(native.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return Common::String();
	Common::String stamp = Common::String::format("%s:%llu:%llu:%llu:%lld:%ld:%lld:%ld", native.c_str(),
		(unsigned long long)info.st_dev, (unsigned long long)info.st_ino, (unsigned long long)info.st_size,
		(long long)info.st_mtim.tv_sec, (long)info.st_mtim.tv_nsec,
		(long long)info.st_ctim.tv_sec, (long)info.st_ctim.tv_nsec);
	return digestText(stamp);
}

static bool validDigest(const Common::String &value) {
	if (value == "P") return true; // Original uncompressed PCM, no conversion.
	if (value.size() != 32) return false;
	for (uint i = 0; i < value.size(); ++i)
		if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
	return true;
}

SSTSourceDigestCache::SSTSourceDigestCache() : _verify(ConfMan.hasKey("sst_verify_cache_sources") && ConfMan.getBool("sst_verify_cache_sources")), _dirty(false) {
	Common::FSNode dir = Common::FSNode(SSTCacheRoot()).getChild("source-digests-v2");
	if (!dir.exists() && !dir.createDirectory()) return;
	_directory = dir.getPath();
	Common::FSNode node(_directory.appendComponent("index.txt"));
	Common::File file;
	if (!node.exists() || !file.open(node) || file.size() > 16 * 1024 * 1024) return;
	while (!file.eos() && !file.err()) {
		Common::String line = file.readLine();
		size_t split = line.find(' ');
		if (split == Common::String::npos) continue;
		Common::String key = line.substr(0, split), value = line.substr(split + 1);
		if (key.size() <= 80 && validDigest(value)) _digests[key] = value;
	}
}

SSTSourceDigestCache::~SSTSourceDigestCache() {
	if (!_dirty || _directory.empty()) return;
	Common::String target = _directory.appendComponent("index.txt").toString(Common::Path::kNativeSeparator);
	Common::String temp = Common::String::format("%s.tmp-%lu", target.c_str(), (unsigned long)getpid());
	FILE *file = fopen(temp.c_str(), "wb");
	if (!file) return;
	bool ok = true;
	for (Common::HashMap<Common::String, Common::String>::const_iterator i = _digests.begin(); i != _digests.end(); ++i) {
		Common::String line = i->_key + " " + i->_value + "\n";
		if (fwrite(line.c_str(), 1, line.size(), file) != line.size()) ok = false;
	}
	if (fclose(file) != 0) ok = false;
	if (ok) ok = rename(temp.c_str(), target.c_str()) == 0;
	if (!ok) remove(temp.c_str());
}

bool SSTSourceDigestCache::read(const Common::String &key, Common::String &digest) const {
	if (_verify || key.empty() || !_digests.contains(key)) return false;
	digest = _digests[key];
	return true;
}

void SSTSourceDigestCache::write(const Common::String &key, const Common::String &digest) {
	if (key.empty() || _directory.empty() || !validDigest(digest)) return;
	_digests[key] = digest;
	_dirty = true;
}

static Common::Path resolveOriginal(const Common::Path &input) {
	Common::Path path = input;
	Common::String native = path.toString('/');
	if (native.empty() || native[0] != '/') path = ConfMan.getPath("path").join(path);
	if (Common::FSNode(path).exists()) return path;
	Common::FSList files;
	Common::FSNode(ConfMan.getPath("path")).getChildren(files, Common::FSNode::kListFilesOnly);
	Common::String name = Common::FSNode(input).getName();
	for (uint i = 0; i < files.size(); ++i)
		if (files[i].getName().equalsIgnoreCase(name)) return files[i].getPath();
	return Common::Path();
}

static Common::String audioSourceGeneration() {
	Common::Array<Common::String> stamps;
	for (size_t i = 0; i < _GP(AssetMgr)->GetLibraryCount(); ++i) {
		const Shared::AssetLibInfo *lib = _GP(AssetMgr)->GetLibraryInfo(i);
		if (lib->LibFileNames.empty()) {
			Common::FSNode dir(resolveOriginal(Common::Path(lib->BasePath.GetCStr())));
			if (!dir.exists() || !dir.isDirectory()) return Common::String();
			Common::FSList files;
			dir.getChildren(files, Common::FSNode::kListFilesOnly);
			for (uint n = 0; n < files.size(); ++n) {
				Common::String name = files[n].getName(); name.toLowercase();
				if (!name.hasSuffix(".ogg") && !name.hasSuffix(".mp3") && !name.hasSuffix(".wav")) continue;
				Common::String stamp = SSTFileMetadataKey(files[n].getPath());
				if (stamp.empty()) return Common::String();
				stamps.push_back(stamp);
			}
		} else {
			for (size_t n = 0; n < lib->LibFileNames.size(); ++n) {
				Common::Path path = resolveOriginal(Common::Path(lib->BaseDir.GetCStr()).join(lib->LibFileNames[n].GetCStr()));
				if (path.empty()) return Common::String();
				Common::String stamp = SSTFileMetadataKey(path);
				if (stamp.empty()) return Common::String();
				stamps.push_back(stamp);
			}
		}
	}
	Common::sort(stamps.begin(), stamps.end());
	Common::String identity("audio-source-index-v1:");
	for (uint i = 0; i < stamps.size(); ++i) identity += stamps[i];
	return digestText(identity);
}

bool PrepareSSTAudioCache() {
	s_pcmFiles.clear();
	if (ConfMan.hasKey("sst_prepare_audio") && !ConfMan.getBool("sst_prepare_audio")) return true;
	SSTPreparationScreen screen;
	if (!screen.update("Finding original audio", 0, 1, "", true)) return false;
	Common::FSNode root(SSTCacheRoot());
	Common::FSNode dir = root.getChild("pcm-v1");
	if ((!dir.exists() && !dir.createDirectory()) || !dir.isDirectory())
		error("Cannot create PCM cache directory");
	std::vector<Shared::AssetPath> assets;
	std::vector<Shared::AssetPath> groups[2];
	const char *filters[] = {"audio", "voice"};
	for (int f = 0; f < 2; ++f) {
		std::vector<Shared::String> names;
		_GP(AssetMgr)->FindAssets(names, "*", filters[f]);
		for (size_t i = 0; i < names.size(); ++i) {
			Common::String lower(names[i].GetCStr()); lower.toLowercase();
			if (lower.hasSuffix(".ogg") || lower.hasSuffix(".mp3") || lower.hasSuffix(".wav"))
				groups[f].push_back(Shared::AssetPath(names[i], filters[f]));
		}
	}
	/* Alternate libraries so short voice clips and long music both make
	 * progress from the start. Cache identity is independent of order. */
	for (size_t i = 0; i < MAX(groups[0].size(), groups[1].size()); ++i)
		for (int group = 0; group < 2; ++group)
			if (i < groups[group].size()) assets.push_back(groups[group][i]);
	SSTSourceDigestCache digests;
	Common::String generation = audioSourceGeneration();
	warning("[sst-pcm-cache] source-index=%s", generation.empty() ? "unavailable" : generation.c_str());
	int hits = 0, decoded = 0, pcmOriginal = 0;
	for (size_t i = 0; i < assets.size(); ++i) {
		if (i < 10 || (i % 100) == 0) warning("[sst-pcm-cache] checked=%u/%u decoded=%d reused=%d", (uint)i, (uint)assets.size(), decoded, hits);
		const Shared::AssetPath &asset = assets[i];
		if (!screen.update("Checking original audio", i, assets.size(), asset.Name.GetCStr())) return false;
		Common::String sourceIndexKey = generation.empty() ? Common::String() : "asset-" + digestText(generation + ":" + assetKey(asset));
		Common::String hash;
		bool knownHash = digests.read(sourceIndexKey, hash);
		if (knownHash && hash == "P") { ++pcmOriginal; continue; }
		if (knownHash) {
			Common::Path output = dir.getPath().appendComponent(digestText(assetKey(asset) + ":" + hash) + ".wav");
			if (validPCM(output)) { ++hits; s_pcmFiles[assetKey(asset)] = output; continue; }
		}
		Shared::Stream *source = _GP(AssetMgr)->OpenAsset(asset.Name, asset.Filter);
		if (!source) error("Cannot read original audio %s", asset.Name.GetCStr());
		Common::SeekableReadStream *data = Common::wrapBufferedSeekableReadStream(new Shared::ScummVMReadStream(source), 65536, DisposeAfterUse::YES);
		byte signature[44];
		uint32 head = data->read(signature, sizeof(signature));
		data->seek(0);
		bool wave = head >= 12 && !memcmp(signature, "RIFF", 4) && !memcmp(signature + 8, "WAVE", 4);
		if (wave && head >= 36 && !memcmp(signature + 12, "fmt ", 4) && READ_LE_UINT16(signature + 20) == 1) {
			++pcmOriginal; digests.write(sourceIndexKey, "P"); delete data; continue;
		}
		HashProgress progress(screen, asset.Name.GetCStr(), i, assets.size());
		if (!knownHash) {
			hash = Common::computeStreamMD5AsString(*data, 0, HashProgress::tick, &progress);
			if (hash.empty()) { delete data; return false; }
			if (data->err() || data->pos() != data->size()) { delete data; error("Read error checking original audio %s", asset.Name.GetCStr()); }
			digests.write(sourceIndexKey, hash);
		}
		data->seek(0);
		Common::String identity = assetKey(asset) + ":" + hash;
		Common::MemoryReadStream id((const byte *)identity.c_str(), identity.size());
		Common::String name = Common::computeStreamMD5AsString(id) + ".wav";
		Common::Path output = dir.getPath().appendComponent(name);
		if (validPCM(output)) { ++hits; delete data; }
		else {
			Audio::AudioStream *audio = nullptr;
			if (wave) {
				audio = Audio::makeWAVStream(data, DisposeAfterUse::YES);
			} else if (head >= 4 && !memcmp(signature, "OggS", 4)) {
#ifdef USE_VORBIS
				audio = Audio::makeVorbisStream(data, DisposeAfterUse::YES);
#endif
			} else {
#ifdef USE_MAD
				audio = Audio::makeMP3Stream(data, DisposeAfterUse::YES);
#endif
			}
			if (!audio) error("Cannot decode original audio %s (Vorbis/MP3 support required)", asset.Name.GetCStr());
			bool ok = writePCM(*audio, output, screen, asset.Name.GetCStr(), i, assets.size());
			delete audio;
			if (!ok) {
				if (g_system->getEventManager()->shouldQuit()) return false;
				error("Cannot finish PCM cache for %s (check free disk space)", asset.Name.GetCStr());
			}
			++decoded;
		}
		s_pcmFiles[assetKey(asset)] = output;
	}
	screen.update("Audio cache ready", assets.size(), assets.size(), "", true);
	warning("[sst-pcm-cache] decoded=%d reused=%d original-wav=%d", decoded, hits, pcmOriginal);
	return true;
}
} } }
