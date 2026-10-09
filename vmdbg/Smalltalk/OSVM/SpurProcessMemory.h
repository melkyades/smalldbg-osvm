#pragma once

#include "SpurMemory.h"
#include <smalldbg/Debugger.h>
#include <string>

namespace osvm {

/// Spur object memory read out of a live OpenSmalltalk VM process.
///
/// Oops in a loaded heap are absolute, so the only bootstrap needed is the
/// address of the interpreter's roots. Those come from the VM's own global
/// symbols; a stripped VM leaves the session with the image file instead.
class SpurProcessMemory : public SpurMemory {
public:
    explicit SpurProcessMemory(smalldbg::Debugger* dbg) { reader = dbg; }

    /// Find the roots of the loaded heap. Tries the interpreter's global
    /// symbols first; released VMs are stripped, so it then scans the target
    /// for the fixed nil/false/true prologue every Spur heap starts with.
    /// The image the VM was started from supplies the reference layout.
    bool locateRoots(uint64_t imageBase = 0, uint64_t imageEnd = 0,
                     uint64_t imageSpecialObjects = 0);

    SpurRoots roots() const override { return vmRoots; }
    uint64_t baseAddress() const override { return base; }
    uint64_t endAddress() const override { return end; }
    bool isLive() const override { return true; }

    const std::string& error() const { return lastError; }

private:
    /// Read a VM global holding an oop, by symbol name.
    bool readGlobalOop(const char* symbol, uint64_t& out) const;
    bool rootsFromSymbols(SpurRoots& found) const;
    /// Locate nil by its header signature, then derive the rest from it.
    bool rootsFromPrologueScan(SpurRoots& found, uint64_t imageBase,
                               uint64_t imageSpecialObjects,
                               std::string& why) const;
    bool prologueMatchesAt(uint64_t address) const;
    uint64_t objectAfter(uint64_t oop) const;
    bool looksLikeClassTable(uint64_t oop) const;

    SpurRoots vmRoots;
    uint64_t base{0};
    uint64_t end{0};
    std::string lastError;
};

} // namespace osvm
