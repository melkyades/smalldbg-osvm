#pragma once

#include "SpurMemory.h"
#include <smalldbg/Debugger.h>
#include <string>

namespace osvm {

/** A SpurProcessMemory reads the Spur object memory of a live OpenSmalltalk VM process.
 *  Oops in a loaded heap are absolute, so only the addresses of the roots are needed. **/
class SpurProcessMemory : public SpurMemory {
public:
    explicit SpurProcessMemory(smalldbg::Debugger* dbg) { reader = dbg; }

    /** Finds the roots of the loaded heap through the interpreter's global symbols or, on a
     *  stripped VM, by scanning for the nil/false/true objects every Spur heap starts with.
     *  The image the VM was started from gives the expected layout. **/
    bool locateRoots(uint64_t imageBase = 0, uint64_t imageEnd = 0,
                     uint64_t imageSpecialObjects = 0);

    SpurRoots roots() const override { return vmRoots; }
    uint64_t baseAddress() const override { return base; }
    uint64_t endAddress() const override { return end; }
    bool isLive() const override { return true; }
    std::pair<uint64_t, uint64_t> allocationContaining(uint64_t address) const override;
    /** Returns the interpreter's newSpaceStart when its symbols name it, and
     *  otherwise the start of the allocation old space was reserved in. **/
    uint64_t newSpaceStart() const override;

    const std::string& error() const { return lastError; }

private:
    /** Reads the oop held by the VM global named `symbol`. **/
    bool readGlobalOop(const char* symbol, uint64_t& out) const;
    bool rootsFromSymbols(SpurRoots& found) const;
    /** Finds nil by its header, then the other roots from it. **/
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
