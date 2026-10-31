#ifndef AGS_ENGINE_SST_PCM_CACHE_H
#define AGS_ENGINE_SST_PCM_CACHE_H
#include "common/stream.h"
#include "common/path.h"
#include "common/hashmap.h"
#include "common/hash-str.h"
#include "ags/shared/core/asset_manager.h"
namespace AGS3 { namespace AGS { namespace Engine {
Common::SeekableReadStream *OpenSSTCachedAudio(const Shared::AssetPath &asset);
Common::Path SSTCacheRoot();
Common::String SSTFileMetadataKey(const Common::Path &path);
class SSTSourceDigestCache {
public:
	SSTSourceDigestCache();
	~SSTSourceDigestCache();
	bool read(const Common::String &key, Common::String &digest) const;
	void write(const Common::String &key, const Common::String &digest);
private:
	Common::Path _directory;
	bool _verify;
	bool _dirty;
	Common::HashMap<Common::String, Common::String> _digests;
};
bool PrepareSSTAudioCache();
} } }
#endif
