#ifndef FILEMANAGER_SLICEFADES_H_
#define FILEMANAGER_SLICEFADES_H_

#include "mtStructs.h"

#include <stdint.h>

class FileSystem;

// Per-slice fade-in / fade-out times for Slice and Beat Slice play modes.
//
// Stored in one sidecar file per project ("sliceFades") so the .pti instrument format stays
// unchanged and stock firmware / Polyend tools remain compatible. Each instrument record keeps a
// fingerprint of the slice layout the fades were written for; if the slices are replaced from
// outside the slice editor (sample import, instrument load, MOD import...), the fingerprint no
// longer matches and the stale fades are ignored instead of being applied to the wrong slices.
class SliceFades
{
  public:
    struct Fade
    {
        uint16_t inMs;
        uint16_t outMs;
    };

    static constexpr uint16_t kMaxFadeMs = 1000;

    void init(FileSystem* fileSystem);

    // Workspace / project persistence
    void loadWorkspace();
    void flushWorkspace();
    bool copyFile(const char* src, const char* dest); // project open: project -> workspace
    void saveToFile(const char* path);                // project save: RAM -> project

    // Playback query (called at note-on). Returns {0,0} if no valid fade is stored.
    Fade getForPlayback(uint8_t instrIdx, uint8_t sliceIdx) const;

    // Editor
    Fade get(uint8_t instrIdx, uint8_t sliceIdx);
    void set(uint8_t instrIdx, uint8_t sliceIdx, Fade fade);
    void setAll(uint8_t instrIdx, Fade fade);

    // Slice list changes: call prepareEdit() before modifying the slice array and one of the
    // on...() hooks after it.
    void prepareEdit(uint8_t instrIdx);
    void onSliceInserted(uint8_t instrIdx, uint8_t newSliceIdx);
    void onSliceRemoved(uint8_t instrIdx, uint8_t removedSliceIdx);
    void onSlicesMoved(uint8_t instrIdx);
    void onSlicesRebuilt(uint8_t instrIdx, Fade fade);

    void copyInstrument(uint8_t srcIdx, uint8_t destIdx);
    void clearInstrument(uint8_t instrIdx);

  private:
    bool isValid(uint8_t instrIdx) const;
    void markChanged(uint8_t instrIdx);
    static uint32_t fingerprint(const strInstrument& instr);
    bool readFile(const char* path);
    void writeFile(const char* path);
    void clearAll();

    FileSystem* fileSystem = nullptr;
    bool dirty = false;
};

extern SliceFades sliceFades;

#endif /* FILEMANAGER_SLICEFADES_H_ */
