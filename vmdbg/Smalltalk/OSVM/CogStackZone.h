#pragma once

#include <smalldbg/Thread.h>
#include <cstdint>
#include <memory>

namespace osvm {

class SpurImage;

/** A CogStackZone finds where a Cog VM is running, on a live target. The VM keeps the
 *  running process's frame pointer in a global; a VM without symbols is searched for
 *  the data word that reads as a stack page's frames. The zone itself is allocated on
 *  the VM thread's own stack, which tells that thread apart. **/
class CogStackZone {
public:
    explicit CogStackZone(const SpurImage* image) : image(image) {}

    /** Returns the frame pointer the running process is at, or 0 when nothing reads as
     *  one. A VM stopped in machine code holds it in the frame pointer register. **/
    uint64_t activeFramePointer();
    /** Returns the thread whose stack holds `framePointer`, or null. **/
    std::shared_ptr<smalldbg::Thread> threadHolding(uint64_t framePointer) const;

private:
    /** Returns true when a page's frames lead from `framePointer` to its base frame. **/
    bool readsAsStackPage(uint64_t framePointer) const;
    uint64_t globalAddressBySymbol() const;
    uint64_t globalAddressByScan() const;

    const SpurImage* image;
    uint64_t globalAddress{0};
    // Scanning reads every module's data, so a failed scan waits for the target to move.
    uint64_t scannedVersion{UINT64_MAX};
};

} // namespace osvm
