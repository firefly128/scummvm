/* Small derived-data cache. No object/script pointers and no serialized state. */
#ifndef SCI_SST_SELECTOR_INDEX_CACHE_H
#define SCI_SST_SELECTOR_INDEX_CACHE_H
namespace Sci {
class SSTSelectorIndexCache {
    mutable unsigned int _entries[16];
    mutable unsigned int _valid;
public:
    SSTSelectorIndexCache() : _valid(0) {
        static_assert(sizeof(unsigned int) == 4, "32-bit packed cache entries required");
        for (unsigned int i = 0; i < 16; ++i) _entries[i] = 0;
    }
    void clear() const { _valid = 0; }
    bool find(int selector, int &index) const {
        if (selector < 0 || selector > 65535) return false;
        const unsigned int slot = (unsigned int)selector & 15;
        if (!(_valid & (1U << slot))) return false;
        const unsigned int value = _entries[slot];
        if ((value >> 16) != (unsigned int)selector) return false;
        index = (int)(value & 65535U) - 1;
        return true;
    }
    void store(int selector, int index) const {
        if (selector < 0 || selector > 65535 || index < -1 || index >= 65535) return;
        const unsigned int slot = (unsigned int)selector & 15;
        _entries[slot] = ((unsigned int)selector << 16) | (unsigned int)(index + 1);
        _valid |= 1U << slot;
    }
};
}
#endif
