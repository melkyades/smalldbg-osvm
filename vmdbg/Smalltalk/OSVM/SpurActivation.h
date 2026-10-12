#pragma once

#include "CogFrame.h"
#include "SpurKnownObjects.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace osvm {

class SpurImage;

/** A SpurActivation is one activation of a process as the debugger lists it: a Context
 *  on the heap, or a frame in a Cog VM's stack zone, which has a Context as well once
 *  the VM was asked for one. **/
class SpurActivation {
public:
    explicit SpurActivation(SpurContext context) : heapContext(context) {}
    explicit SpurActivation(CogFrame frame) : stackFrame(frame), heapContext(frame.context()) {}

    bool isFrame() const { return stackFrame.isValid(); }
    const CogFrame& frame() const { return stackFrame; }
    /** Returns the activation's context, or none for a frame without one. **/
    SpurContext context() const { return heapContext; }

    SpurCompiledMethod method() const;
    SpurObject receiver() const;
    bool isBlock() const;
    std::vector<SpurObject> temporaries() const;

private:
    CogFrame stackFrame;
    SpurContext heapContext;
};

/** Returns a process's activations, from the frame at `framePointer` when the process
 *  is running and from its suspended context `top` otherwise. A married context is read
 *  through its frame, and each stack page's base frame leads to the context it returns
 *  to. Stops after `limit`. **/
std::vector<SpurActivation> activationsFrom(SpurContext top, uint64_t framePointer,
                                            const SpurImage* image, size_t limit);

} // namespace osvm
