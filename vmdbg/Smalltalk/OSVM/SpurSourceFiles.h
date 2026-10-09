#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace osvm {

/// The .sources / .changes pair a Cuis image stores its method text in.
///
/// A CompiledMethod ends in a 4-byte trailer whose last byte is >= 252; the
/// source pointer is that byte minus 251 followed by the three preceding
/// bytes, most significant first. Bit 16r2000000 of the resulting pointer
/// selects .changes over .sources, and the remaining offset is scaled by
/// CuisSourceFileArray's pointerScale so 25 bits can address both files.
class SpurSourceFiles {
public:
    /// Locate the files that go with `imagePath`, preferring the .changes of
    /// the same stem and the newest .sources beside it.
    void locateFor(const std::string& imagePath);

    bool loadSources(const std::string& path);
    bool loadChanges(const std::string& path);

    /// The chunk starting at `sourcePointer`, without its trailing bang.
    std::string chunkAt(uint64_t sourcePointer) const;

    /// The chunk at a raw file position. RemoteString (used for class
    /// comments) stores an unscaled position and its own file number rather
    /// than an encoded source pointer.
    std::string chunkIn(uint32_t fileIndex, uint64_t position) const;

    bool haveSources() const { return !sources.empty(); }
    bool haveChanges() const { return !changes.empty(); }
    const std::string& sourcesPath() const { return sourcesFile; }
    const std::string& changesPath() const { return changesFile; }

    static constexpr uint64_t POINTER_SCALE  = 32;
    static constexpr uint64_t POINTER_BASE   = 0x1000000;
    static constexpr uint64_t CHANGES_FLAG   = 0x2000000;
    static constexpr uint64_t POSITION_MASK  = 0x1FFFFFF;

    static uint32_t fileIndexOf(uint64_t sourcePointer);
    static uint64_t filePositionOf(uint64_t sourcePointer);

private:
    static bool readWhole(const std::string& path, std::string& out);

    std::string sources;
    std::string changes;
    std::string sourcesFile;
    std::string changesFile;
};

} // namespace osvm
