#pragma once

#include <smalldbg/Debugger.h>

#include <cstdint>
#include <cstddef>

namespace osvm {

/** SpurRoots holds the objects needed to start reading a Spur heap. An image file finds
 *  them at the start of its heap; a live VM reads them from the interpreter's globals.
 *  Oops are absolute in both cases, so nothing needs relocating once these are known. **/
struct SpurRoots {
    uint64_t nilObj{0};
    uint64_t falseObj{0};
    uint64_t trueObj{0};
    uint64_t hiddenRootsObj{0};
    uint64_t specialObjectsOop{0};
    bool valid{false};
};

/** A SpurMemory reads a Spur object memory, its roots and its bounds through a debugger,
 *  either one opened on an image file (SpurImageFile) or one attached to a live VM
 *  (SpurProcessMemory). Code above it uses absolute oops and does not know which. **/
class SpurMemory {
public:
    virtual ~SpurMemory() = default;

    smalldbg::Debugger* debugger() const { return reader; }

    bool readBytes(uint64_t address, void* out, size_t size) const {
        return reader->readMemory(address, out, size) == smalldbg::Status::Ok;
    }

    virtual SpurRoots roots() const = 0;

    /** Returns the first address of the readable old space. **/
    virtual uint64_t baseAddress() const = 0;
    /** Returns the address just past the readable old space. A live heap reports the end of
     *  the image it loaded; objects allocated past it can be read but heap walks stop there. **/
    virtual uint64_t endAddress() const = 0;

    virtual bool isLive() const { return false; }

    /** Returns the word at `address`, or 0 when it cannot be read. **/
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
