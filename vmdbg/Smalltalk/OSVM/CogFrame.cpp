#include "CogFrame.h"
#include "CogFormat.h"
#include "SpurImage.h"

namespace osvm {

namespace {

// More frames than a stack page can hold, so a corrupt chain still ends.
constexpr size_t MAX_FRAMES_PER_PAGE = 4096;

uint8_t byteOf(uint64_t word, int index) {
    return static_cast<uint8_t>(word >> (8 * index));
}

} // namespace

CogFrame::CogFrame(uint64_t framePointer, const SpurImage* spurImage)
    : fp(framePointer), image(spurImage) {
    if (fp == 0 || fp % Spur::BYTES_PER_OOP != 0) return;
    methodField = word(Cog::METHOD);
    if (isMethodOop(methodField)) readInterpreterFrame();
    else readMachineCodeFrame();
}

uint64_t CogFrame::readWord(uint64_t address) const {
    return image->memoryReader()->readWord(address);
}

uint64_t CogFrame::word(int64_t offset) const {
    return readWord(fp + offset);
}

bool CogFrame::isMethodOop(uint64_t oop) const {
    if (oop == 0 || oop % Spur::BYTES_PER_OOP != 0) return false;
    if (!Fmt::isMethod(image->formatOf(oop))) return false;
    const uint64_t compiledMethod = image->specialObject(SpecialObject::COMPILED_METHOD);
    for (SpurBehavior cls(image->heapObjectAt(image->classOf(oop))); cls; cls = cls.superclass())
        if (cls.oop() == compiledMethod) return true;
    return false;
}

void CogFrame::readInterpreterFrame() {
    methodOop = methodField;
    interpreterFlags = word(Cog::IFRAME_FLAGS);
    argumentCount = byteOf(interpreterFlags, Cog::IFRAME_NUM_ARGS_BYTE);
}

// Reads the method of a frame whose method field points at machine code. A
// block's code sits inside its home method's, which knows the CompiledMethod,
// and that method's header slot points back at the code.
void CogFrame::readMachineCodeFrame() {
    const uint64_t code = methodField & ~Cog::MFRAME_FLAGS_MASK;
    if (code == 0) return;
    const uint64_t codeFlags = readWord(code + Cog::COG_FLAGS);
    uint64_t home = code;
    if ((methodField & Cog::MFRAME_IS_BLOCK) != 0 &&
        ((codeFlags >> Cog::COG_TYPE_SHIFT) & Cog::COG_TYPE_MASK) < Cog::CM_METHOD)
        home = code - (readWord(code + Cog::COG_HOME_OFFSET) & Cog::COG_HOME_MASK);

    const uint64_t candidate = readWord(home + Cog::COG_METHOD_OBJECT);
    if (!isMethodOop(candidate)) return;
    if (image->slotOf(candidate, SpurCompiledMethod::Slot::HEADER) != home) return;
    methodOop = candidate;
    homeCode = home;
    machineCode = true;
    argumentCount = static_cast<uint32_t>(codeFlags & Cog::COG_NUM_ARGS_MASK);
}

uint64_t CogFrame::codeStart() const {
    return machineCode ? homeCode + Cog::COG_METHOD_SIZE : 0;
}

uint64_t CogFrame::returnAddress() const {
    return word(Cog::CALLER_SAVED_IP);
}

bool CogFrame::isBlock() const {
    if (machineCode) return (methodField & Cog::MFRAME_IS_BLOCK) != 0;
    return byteOf(interpreterFlags, Cog::IFRAME_IS_BLOCK_BYTE) != 0;
}

bool CogFrame::hasContext() const {
    if (machineCode) return (methodField & Cog::MFRAME_HAS_CONTEXT) != 0;
    return byteOf(interpreterFlags, Cog::IFRAME_HAS_CONTEXT_BYTE) != 0;
}

SpurCompiledMethod CogFrame::method() const {
    return image->methodAt(methodOop);
}

int64_t CogFrame::receiverOffset() const {
    return machineCode ? Cog::MFRAME_RECEIVER : Cog::IFRAME_RECEIVER;
}

int64_t CogFrame::stackedReceiverOffset() const {
    return Cog::CALLER_SAVED_IP + static_cast<int64_t>(Spur::BYTES_PER_OOP) * (1 + argumentCount);
}

SpurObject CogFrame::receiver() const {
    return image->objectAt(word(receiverOffset()));
}

SpurContext CogFrame::context() const {
    if (!hasContext()) return SpurContext();
    return SpurContext(image->heapObjectAt(word(Cog::THIS_CONTEXT)));
}

uint64_t CogFrame::localCount() const {
    if (isBlock()) {
        const SpurHeapObject closure = image->heapObjectAt(word(stackedReceiverOffset()));
        const uint64_t fixed = SpurBehavior(closure.spurClass()).fixedFields();
        return closure.numSlots() > fixed ? closure.numSlots() - fixed : 0;
    }
    const uint32_t temps = method().header().numTemps;
    return temps > argumentCount ? temps - argumentCount : 0;
}

std::vector<SpurObject> CogFrame::temporaries() const {
    std::vector<SpurObject> found;
    const int64_t wordSize = static_cast<int64_t>(Spur::BYTES_PER_OOP);
    for (uint32_t i = 0; i < argumentCount; i++)
        found.push_back(image->objectAt(word(stackedReceiverOffset() - wordSize * (1 + i))));
    const uint64_t locals = localCount();
    for (uint64_t i = 0; i < locals; i++)
        found.push_back(image->objectAt(word(receiverOffset() - wordSize * static_cast<int64_t>(1 + i))));
    return found;
}

uint64_t CogFrame::lowestAddress() const {
    return fp + receiverOffset() - Spur::BYTES_PER_OOP * localCount();
}

uint64_t CogFrame::highestAddress() const {
    const uint64_t pushed = fp + stackedReceiverOffset();
    return isBase() ? pushed + 2 * Spur::BYTES_PER_OOP : pushed;
}

std::string CogFrame::roleOf(uint64_t address) const {
    const int64_t offset = static_cast<int64_t>(address - fp);
    const int64_t wordSize = static_cast<int64_t>(Spur::BYTES_PER_OOP);
    if (offset == Cog::SAVED_FP) return "fp";
    if (offset == Cog::CALLER_SAVED_IP) return "retAddr";
    if (offset == Cog::METHOD) return "method";
    if (offset == Cog::THIS_CONTEXT) return "context";
    if (offset == receiverOffset()) return "receiver";
    if (!machineCode && offset == Cog::IFRAME_FLAGS) return "flags";
    if (!machineCode && offset == Cog::IFRAME_SAVED_IP) return "savedIP";
    if (offset == stackedReceiverOffset()) return isBlock() ? "closure" : "pushed receiver";
    if (isBase() && offset == stackedReceiverOffset() + wordSize) return "base context";
    if (isBase() && offset == stackedReceiverOffset() + 2 * wordSize) return "caller context";
    if (offset > Cog::CALLER_SAVED_IP && offset < stackedReceiverOffset()) return "arg";
    if (offset < receiverOffset() && address >= lowestAddress()) return "temp";
    return {};
}

bool CogFrame::holdsObject(uint64_t address) const {
    const std::string role = roleOf(address);
    return role != "fp" && role != "retAddr" && role != "flags" && role != "savedIP" &&
           !(role == "method" && machineCode) && !(role == "context" && !hasContext());
}

bool CogFrame::isBase() const {
    return word(Cog::SAVED_FP) == 0;
}

CogFrame CogFrame::caller() const {
    if (isBase()) return CogFrame();
    return CogFrame(word(Cog::SAVED_FP), image);
}

// A base frame is pushed below the context it returns to and the context it
// was made for, which sit at the bottom of the page.
SpurContext CogFrame::callerContext() const {
    if (!isBase()) return SpurContext();
    const uint64_t oop = word(stackedReceiverOffset() + 2 * static_cast<int64_t>(Spur::BYTES_PER_OOP));
    if (!image->isPresent(oop)) return SpurContext();
    return SpurContext(image->heapObjectAt(oop));
}

std::vector<CogFrame> framesFrom(uint64_t framePointer, const SpurImage* image) {
    std::vector<CogFrame> frames;
    CogFrame frame(framePointer, image);
    while (frame.isValid() && frames.size() < MAX_FRAMES_PER_PAGE) {
        frames.push_back(frame);
        if (frame.isBase()) break;
        frame = frame.caller();
    }
    return frames;
}

} // namespace osvm
