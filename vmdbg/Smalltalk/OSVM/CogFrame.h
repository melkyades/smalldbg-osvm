#pragma once

#include "SpurKnownObjects.h"
#include <cstdint>
#include <string>
#include <vector>

namespace osvm {

class SpurImage;

/** A CogFrame is one activation in a Cog VM's stack zone, where the VM runs a
 *  process's methods until something needs them as Context objects. It is an
 *  interpreter frame when its method field holds a CompiledMethod, and a machine
 *  code frame when it holds the address of the code compiled from one. **/
class CogFrame {
public:
    CogFrame() = default;
    CogFrame(uint64_t framePointer, const SpurImage* image);

    /** Returns true when the words at the frame pointer read as a frame: its method
     *  field names a CompiledMethod, directly or through the code compiled from it. **/
    bool isValid() const { return methodOop != 0; }
    uint64_t framePointer() const { return fp; }
    bool isMachineCode() const { return machineCode; }
    /** Returns where a machine code frame's method's code starts, or 0. **/
    uint64_t codeStart() const;
    /** Returns where the frame returns to in its caller. **/
    uint64_t returnAddress() const;
    bool isBlock() const;
    bool hasContext() const;
    uint32_t numArgs() const { return argumentCount; }
    /** Returns the method the frame runs, or the one a block's code is part of. **/
    SpurCompiledMethod method() const;
    SpurObject receiver() const;
    /** Returns the Context married to the frame, or none when nothing asked for one. **/
    SpurContext context() const;
    /** Returns the arguments, then the values a block copied or a method's temporaries. **/
    std::vector<SpurObject> temporaries() const;

    /** Returns the lowest address the frame's own slots take, its last local. **/
    uint64_t lowestAddress() const;
    /** Returns the highest address the frame's slots take: where the receiver was pushed
     *  for the call, or the bottom of the page for a base frame. **/
    uint64_t highestAddress() const;
    /** Returns what the slot at `address` holds for the frame, such as "receiver", or an
     *  empty string for a value on its operand stack. **/
    std::string roleOf(uint64_t address) const;
    /** Returns true when the slot at `address` holds an object rather than a VM word. **/
    bool holdsObject(uint64_t address) const;

    /** Returns true when the frame is the first one on its stack page. **/
    bool isBase() const;
    /** Returns the calling frame, which is on the same stack page unless this is a base frame. **/
    CogFrame caller() const;
    /** Returns the context a base frame returns to, or none at the bottom of a process. **/
    SpurContext callerContext() const;

private:
    uint64_t word(int64_t offset) const;
    uint64_t readWord(uint64_t address) const;
    bool isMethodOop(uint64_t oop) const;
    void readInterpreterFrame();
    void readMachineCodeFrame();
    /** Returns where the receiver, or a block's closure, was pushed for the call. **/
    int64_t stackedReceiverOffset() const;
    int64_t receiverOffset() const;
    /** Returns how many values sit below the receiver: a method's temporaries, or the
     *  values a block copied. **/
    uint64_t localCount() const;

    uint64_t fp{0};
    const SpurImage* image{nullptr};
    uint64_t methodField{0};
    uint64_t methodOop{0};
    uint64_t homeCode{0};
    uint64_t interpreterFlags{0};
    uint32_t argumentCount{0};
    bool machineCode{false};
};

/** Returns the frames of a stack page from the one at `framePointer` to the page's
 *  base frame. Returns none when the first does not read as a frame. **/
std::vector<CogFrame> framesFrom(uint64_t framePointer, const SpurImage* image);

} // namespace osvm
