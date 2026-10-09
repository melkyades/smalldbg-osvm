#pragma once

#include <smalldbg/Debugger.h>

#include <cstdint>
#include <cstddef>

namespace osvm {

/// Roots needed to bootstrap an object model out of a Spur heap.
///
/// A heap read from an image file derives these by walking the fixed
/// prologue; a heap read from a live VM reads them from the interpreter's
/// global symbols. Oops are absolute in both cases, so nothing else has to
/// be rebased once these are known.
struct SpurRoots {
    uint64_t nilObj{0};
    uint64_t falseObj{0};
    uint64_t trueObj{0};
    uint64_t hiddenRootsObj{0};
    uint64_t specialObjectsOop{0};
    bool valid{false};
};

/// A Spur object memory: where its roots are and how far it reaches, read
/// through a debugger.
///
/// The debugger is either one built over an image file (SpurImageFile) or the
/// one attached to a live VM (SpurProcessMemory). Everything above this works
/// in absolute oops and never learns which one it is reading.
class SpurMemory {
public:
    virtual ~SpurMemory() = default;

    smalldbg::Debugger* debugger() const { return reader; }

    bool readBytes(uint64_t address, void* out, size_t size) const {
        return reader->readMemory(address, out, size) == smalldbg::Status::Ok;
    }

    virtual SpurRoots roots() const = 0;

    /// Inclusive-exclusive bounds of the readable old-space range. A live
    /// heap reports the image extent it was loaded from; objects allocated
    /// past it are still readable, they just do not bound heap walks.
    virtual uint64_t baseAddress() const = 0;
    virtual uint64_t endAddress() const = 0;

    virtual bool isLive() const { return false; }

    /** The word at `address`. Returns zero when it cannot be read. **/
    uint64_t readWord(uint64_t address) const {
        uint64_t word = 0;
        if (!readBytes(address, &word, sizeof(word))) return 0;
        return word;
    }

    bool contains(uint64_t address) const {
        return address >= baseAddress() && address < endAddress();
    }

protected:
    smalldbg::Debugger* reader{nullptr};
};

} // namespace osvm
