#pragma once

#include "SpurMemory.h"

#include <memory>
#include <string>

namespace osvm {

/** A SpurImageFile reads a 64-bit Spur image file through a debugger opened on it.
 *  The little-endian header has a 4-byte version, a 4-byte header size, then 8-byte
 *  fields: data size, old base address, special objects oop, last hash, saved window
 *  size, header flags, extra VM memory. Object data starts at `headerSize` and maps
 *  linearly onto `oldBaseAddr`. **/
class SpurImageFile : public SpurMemory {
public:
    bool open(const std::string& path);

    SpurRoots roots() const override { return imageRoots; }
    uint64_t baseAddress() const override { return oldBaseAddr; }
    uint64_t endAddress() const override { return oldBaseAddr + dataSize; }

    uint32_t version() const { return imageVersion; }
    uint64_t specialObjectsOop() const { return specialObjects; }
    const std::string& path() const { return imagePath; }
    const std::string& error() const { return lastError; }

private:
    /** Finds nil, false, true, the free lists and the hidden roots, which come first in
     *  the image in that order. **/
    void deriveRoots();
    uint64_t objectAfter(uint64_t oop) const;

    std::string imagePath;
    std::string lastError;
    std::unique_ptr<smalldbg::Debugger> snapshot;

    uint32_t imageVersion{0};
    uint32_t headerSize{0};
    uint64_t dataSize{0};
    uint64_t oldBaseAddr{0};
    uint64_t specialObjects{0};
    SpurRoots imageRoots;
};

} // namespace osvm
