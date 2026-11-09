#include "SliceFades.h"

#include "fileManager.h"
#include "fileManagerDefs.h"

#include <cr_section_macros.h>
#include <string.h>

SliceFades sliceFades;

namespace
{
constexpr char kFileId[4] = { 'S', 'L', 'F', 'D' };
constexpr uint16_t kFileVersion = 1;

struct SliceFadesFile
{
    struct Header
    {
        char id[4];
        uint16_t version;
        uint16_t instrumentCount;
        uint16_t sliceCount;
        uint16_t reserved;
        uint32_t dataSize;
    } header;

    struct InstrumentRecord
    {
        uint32_t fingerprint; // slice layout the fades belong to
        uint8_t used;         // 0 = no fades stored for this instrument
        uint8_t reserved[3];
        SliceFades::Fade fade[MAX_SLICE_NUMBER];
    } instrument[kSampleInstrumentsCount];
};

__NOINIT(EXTERNAL_RAM)
SliceFadesFile externalRamSliceFades;

SliceFadesFile::InstrumentRecord& record(uint8_t instrIdx)
{
    return externalRamSliceFades.instrument[instrIdx];
}

SliceFades::Fade clampFade(SliceFades::Fade fade)
{
    if (fade.inMs > SliceFades::kMaxFadeMs)
        fade.inMs = SliceFades::kMaxFadeMs;
    if (fade.outMs > SliceFades::kMaxFadeMs)
        fade.outMs = SliceFades::kMaxFadeMs;
    return fade;
}
} // namespace

void SliceFades::init(FileSystem* fileSystem)
{
    this->fileSystem = fileSystem;
    clearAll();
}

void SliceFades::clearAll()
{
    memset(&externalRamSliceFades, 0, sizeof(externalRamSliceFades));
    dirty = false;
}

//==================================================================================================
// Persistence

bool SliceFades::readFile(const char* path)
{
    if (fileSystem == nullptr || !fileSystem->exists(path))
    {
        return false;
    }

    auto openFileInfo = fileSystem->openFile(path);
    if (!std::get<0>(openFileInfo))
    {
        return false;
    }
    auto file = std::get<1>(openFileInfo);

    int32_t bytesRead =
      fileSystem->readFile(&externalRamSliceFades, sizeof(externalRamSliceFades), file);
    fileSystem->closeFile(file);

    const SliceFadesFile::Header& h = externalRamSliceFades.header;
    if (bytesRead != (int32_t)sizeof(externalRamSliceFades) || memcmp(h.id, kFileId, 4) != 0 ||
        h.version != kFileVersion || h.instrumentCount != kSampleInstrumentsCount ||
        h.sliceCount != MAX_SLICE_NUMBER || h.dataSize != sizeof(externalRamSliceFades))
    {
        clearAll();
        return false;
    }

    return true;
}

void SliceFades::writeFile(const char* path)
{
    if (fileSystem == nullptr)
    {
        return;
    }

    SliceFadesFile::Header& h = externalRamSliceFades.header;
    memcpy(h.id, kFileId, 4);
    h.version = kFileVersion;
    h.instrumentCount = kSampleInstrumentsCount;
    h.sliceCount = MAX_SLICE_NUMBER;
    h.reserved = 0;
    h.dataSize = sizeof(externalRamSliceFades);

    auto openFileInfo = fileSystem->openFile(path, FileSystem::FileOpenMode::kWriteOpen);
    if (std::get<0>(openFileInfo))
    {
        auto file = std::get<1>(openFileInfo);
        fileSystem->writeFile(&externalRamSliceFades, sizeof(externalRamSliceFades), file);
        fileSystem->closeFile(file);
    }
}

void SliceFades::loadWorkspace()
{
    if (!readFile(cWorkspaceSliceFadesFilePath))
    {
        clearAll();
    }
    dirty = false;
}

void SliceFades::flushWorkspace()
{
    if (!dirty)
    {
        return;
    }

    newFileManager.createWorkspaceDirs();
    writeFile(cWorkspaceSliceFadesFilePath);
    dirty = false;
}

bool SliceFades::copyFile(const char* src, const char* dest)
{
    bool ok = readFile(src);
    if (ok)
    {
        writeFile(dest);
    }
    else
    {
        clearAll();
        if (fileSystem != nullptr && fileSystem->exists(dest))
        {
            fileSystem->remove(dest);
        }
    }
    dirty = false;
    return ok;
}

void SliceFades::saveToFile(const char* path)
{
    flushWorkspace();

    bool anyUsed = false;
    for (uint8_t i = 0; i < kSampleInstrumentsCount; i++)
    {
        if (record(i).used)
        {
            anyUsed = true;
            break;
        }
    }

    // Projects that never used slice fades don't get the extra file; remove a stale one.
    if (!anyUsed)
    {
        if (fileSystem != nullptr && fileSystem->exists(path))
        {
            fileSystem->remove(path);
        }
        return;
    }

    writeFile(path);
}

//==================================================================================================
// Validity

uint32_t SliceFades::fingerprint(const strInstrument& instr)
{
    // FNV-1a over the slice count and positions
    uint32_t hash = 2166136261u;
    uint8_t count = instr.sliceNumber > MAX_SLICE_NUMBER ? MAX_SLICE_NUMBER : instr.sliceNumber;

    hash = (hash ^ count) * 16777619u;
    for (uint8_t i = 0; i < count; i++)
    {
        hash = (hash ^ (instr.slices[i] & 0xFF)) * 16777619u;
        hash = (hash ^ (instr.slices[i] >> 8)) * 16777619u;
    }
    return hash;
}

bool SliceFades::isValid(uint8_t instrIdx) const
{
    if (instrIdx >= kSampleInstrumentsCount)
        return false;

    const SliceFadesFile::InstrumentRecord& rec = externalRamSliceFades.instrument[instrIdx];
    return rec.used && rec.fingerprint == fingerprint(mtProject.instrument[instrIdx]);
}

void SliceFades::prepareEdit(uint8_t instrIdx)
{
    if (instrIdx >= kSampleInstrumentsCount)
        return;

    SliceFadesFile::InstrumentRecord& rec = record(instrIdx);
    if (rec.used && rec.fingerprint != fingerprint(mtProject.instrument[instrIdx]))
    {
        // Slices were changed outside the slice editor: the stored fades no longer apply.
        memset(&rec, 0, sizeof(rec));
        dirty = true;
    }
}

void SliceFades::markChanged(uint8_t instrIdx)
{
    SliceFadesFile::InstrumentRecord& rec = record(instrIdx);
    rec.used = 1;
    rec.fingerprint = fingerprint(mtProject.instrument[instrIdx]);
    dirty = true;

    newFileManager.setInstrumentStructChanged(instrIdx);
    mtProject.values.projectNotSavedFlag |= 1;
}

//==================================================================================================
// Queries

SliceFades::Fade SliceFades::getForPlayback(uint8_t instrIdx, uint8_t sliceIdx) const
{
    if (sliceIdx >= MAX_SLICE_NUMBER || !isValid(instrIdx))
        return { 0, 0 };

    return externalRamSliceFades.instrument[instrIdx].fade[sliceIdx];
}

SliceFades::Fade SliceFades::get(uint8_t instrIdx, uint8_t sliceIdx)
{
    if (instrIdx >= kSampleInstrumentsCount || sliceIdx >= MAX_SLICE_NUMBER)
        return { 0, 0 };

    prepareEdit(instrIdx);
    return record(instrIdx).fade[sliceIdx];
}

//==================================================================================================
// Edits

void SliceFades::set(uint8_t instrIdx, uint8_t sliceIdx, Fade fade)
{
    if (instrIdx >= kSampleInstrumentsCount || sliceIdx >= MAX_SLICE_NUMBER)
        return;

    prepareEdit(instrIdx);
    record(instrIdx).fade[sliceIdx] = clampFade(fade);
    markChanged(instrIdx);
}

void SliceFades::setAll(uint8_t instrIdx, Fade fade)
{
    if (instrIdx >= kSampleInstrumentsCount)
        return;

    prepareEdit(instrIdx);
    fade = clampFade(fade);
    for (uint8_t i = 0; i < MAX_SLICE_NUMBER; i++)
    {
        record(instrIdx).fade[i] = fade;
    }
    markChanged(instrIdx);
}

void SliceFades::onSliceInserted(uint8_t instrIdx, uint8_t newSliceIdx)
{
    if (instrIdx >= kSampleInstrumentsCount || newSliceIdx >= MAX_SLICE_NUMBER)
        return;

    SliceFadesFile::InstrumentRecord& rec = record(instrIdx);
    if (!rec.used)
        return; // nothing stored, nothing to keep in step

    if (mtProject.instrument[instrIdx].sliceNumber <= 1)
    {
        memset(rec.fade, 0, sizeof(rec.fade));
    }
    else
    {
        // The new slice splits the previous one: shift the rest up, new slice inherits fades.
        for (uint8_t i = MAX_SLICE_NUMBER - 1; i > newSliceIdx; i--)
        {
            rec.fade[i] = rec.fade[i - 1];
        }
        if (newSliceIdx == 0)
            rec.fade[0] = { 0, 0 };
    }
    markChanged(instrIdx);
}

void SliceFades::onSliceRemoved(uint8_t instrIdx, uint8_t removedSliceIdx)
{
    if (instrIdx >= kSampleInstrumentsCount || removedSliceIdx >= MAX_SLICE_NUMBER)
        return;

    SliceFadesFile::InstrumentRecord& rec = record(instrIdx);
    if (!rec.used)
        return;

    for (uint8_t i = removedSliceIdx; i < MAX_SLICE_NUMBER - 1; i++)
    {
        rec.fade[i] = rec.fade[i + 1];
    }
    rec.fade[MAX_SLICE_NUMBER - 1] = { 0, 0 };
    markChanged(instrIdx);
}

void SliceFades::onSlicesMoved(uint8_t instrIdx)
{
    if (instrIdx >= kSampleInstrumentsCount || !record(instrIdx).used)
        return;

    markChanged(instrIdx);
}

void SliceFades::onSlicesRebuilt(uint8_t instrIdx, Fade fade)
{
    if (instrIdx >= kSampleInstrumentsCount)
        return;

    SliceFadesFile::InstrumentRecord& rec = record(instrIdx);
    if (!rec.used && fade.inMs == 0 && fade.outMs == 0)
        return;

    fade = clampFade(fade);
    for (uint8_t i = 0; i < MAX_SLICE_NUMBER; i++)
    {
        rec.fade[i] = fade;
    }
    markChanged(instrIdx);
}

void SliceFades::copyInstrument(uint8_t srcIdx, uint8_t destIdx)
{
    if (srcIdx >= kSampleInstrumentsCount || destIdx >= kSampleInstrumentsCount ||
        srcIdx == destIdx)
        return;

    record(destIdx) = record(srcIdx);
    dirty = true;
}

void SliceFades::clearInstrument(uint8_t instrIdx)
{
    if (instrIdx >= kSampleInstrumentsCount)
        return;

    if (record(instrIdx).used)
    {
        memset(&record(instrIdx), 0, sizeof(SliceFadesFile::InstrumentRecord));
        dirty = true;
    }
}
