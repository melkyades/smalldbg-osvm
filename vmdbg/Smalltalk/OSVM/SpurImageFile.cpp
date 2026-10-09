#include "SpurImageFile.h"
#include "SpurFormat.h"

#include <smalldbg/Arch.h>

#include <fstream>
#include <sstream>
#include <vector>

namespace osvm {

namespace {

constexpr uint32_t SPUR_64_VERSION = 68021;
constexpr uint32_t SPUR_64_VERSION_SWAPPED = 68019;

constexpr size_t OFFSET_VERSION      = 0;
constexpr size_t OFFSET_HEADER_SIZE  = 4;
constexpr size_t OFFSET_DATA_SIZE    = 8;
constexpr size_t OFFSET_OLD_BASE     = 16;
constexpr size_t OFFSET_SPECIAL_OOPS = 24;
constexpr size_t MIN_HEADER_BYTES    = 64;

} // namespace

bool SpurImageFile::open(const std::string& path) {
    imagePath = path;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        lastError = "cannot open image file: " + path;
        return false;
    }

    const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
    if (fileSize < MIN_HEADER_BYTES) {
        lastError = "image file too small";
        return false;
    }
    std::vector<uint8_t> header(MIN_HEADER_BYTES);
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(header.data()), MIN_HEADER_BYTES)) {
        lastError = "failed to read image file";
        return false;
    }

    const uint8_t* raw = header.data();
    imageVersion = *reinterpret_cast<const uint32_t*>(raw + OFFSET_VERSION);
    if (imageVersion != SPUR_64_VERSION) {
        std::ostringstream msg;
        msg << "unsupported image format " << imageVersion
            << " (expected " << SPUR_64_VERSION << ", 64-bit Spur)";
        if (imageVersion == SPUR_64_VERSION_SWAPPED)
            msg << " -- byte-swapped images are not supported";
        lastError = msg.str();
        return false;
    }

    headerSize     = *reinterpret_cast<const uint32_t*>(raw + OFFSET_HEADER_SIZE);
    dataSize       = *reinterpret_cast<const uint64_t*>(raw + OFFSET_DATA_SIZE);
    oldBaseAddr    = *reinterpret_cast<const uint64_t*>(raw + OFFSET_OLD_BASE);
    specialObjects = *reinterpret_cast<const uint64_t*>(raw + OFFSET_SPECIAL_OOPS);

    if (headerSize + dataSize > fileSize) {
        lastError = "image header describes more data than the file holds";
        return false;
    }

    snapshot = std::make_unique<smalldbg::Debugger>(
        smalldbg::Mode::External, smalldbg::X64::instance(), smalldbg::Debuggee::Snapshot);
    if (snapshot->openSnapshot({smalldbg::SnapshotRegion::fromFile(
            oldBaseAddr, path, headerSize, dataSize)}) != smalldbg::Status::Ok) {
        lastError = "failed to read image file";
        return false;
    }
    reader = snapshot.get();

    deriveRoots();
    return imageRoots.valid;
}

uint64_t SpurImageFile::objectAfter(uint64_t oop) const {
    const uint64_t header = readWord(oop);
    uint64_t slots = (header >> Spur::SLOTS_SHIFT) & Spur::SLOTS_OVERFLOW;
    if (slots == Spur::SLOTS_OVERFLOW)
        slots = readWord(oop - Spur::BASE_HEADER_SIZE) & Spur::OVERFLOW_MASK;

    uint64_t next = oop + Spur::objectBytesForSlots(slots);
    const uint64_t nextHeader = readWord(next);
    if (((nextHeader >> Spur::SLOTS_SHIFT) & Spur::SLOTS_OVERFLOW) == Spur::SLOTS_OVERFLOW)
        next += Spur::BASE_HEADER_SIZE;
    return next;
}

void SpurImageFile::deriveRoots() {
    // Spur lays the image out with nil, false, true, the free lists and the
    // hidden roots (the class table) first, in that order.
    imageRoots.nilObj = oldBaseAddr;
    imageRoots.falseObj = objectAfter(imageRoots.nilObj);
    imageRoots.trueObj = objectAfter(imageRoots.falseObj);
    const uint64_t freeLists = objectAfter(imageRoots.trueObj);
    imageRoots.hiddenRootsObj = objectAfter(freeLists);
    imageRoots.specialObjectsOop = specialObjects;

    const uint64_t hiddenHeader = readWord(imageRoots.hiddenRootsObj);
    const uint32_t hiddenFormat =
        static_cast<uint32_t>((hiddenHeader >> Spur::FORMAT_SHIFT) & Spur::FORMAT_MASK);
    imageRoots.valid = hiddenFormat == Fmt::VARIABLE && contains(specialObjects);
    if (!imageRoots.valid)
        lastError = "image prologue does not look like a Spur class table";
}

} // namespace osvm
