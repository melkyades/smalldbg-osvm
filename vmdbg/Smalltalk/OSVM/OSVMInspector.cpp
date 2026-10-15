#include "OSVMInspector.h"
#include "SistaV1Disassembler.h"
#include "CogFrame.h"
#include <smalldbg/Debugger.h>
#include <smalldbg/symbols/SymbolProvider.h>
#include <cstdlib>

namespace osvm {

namespace {

// The global a Cog VM keeps the start of its machine code zone in.
constexpr const char* SYM_METHOD_ZONE_BASE = "methodZoneBase";

// The globals a VM keeps its screen in: a pointer to the pixels, and ints.
constexpr const char* SYM_DISPLAY_BITS = "displayBits";
constexpr const char* SYM_DISPLAY_WIDTH = "displayWidth";
constexpr const char* SYM_DISPLAY_HEIGHT = "displayHeight";
constexpr const char* SYM_DISPLAY_DEPTH = "displayDepth";

// Reads a picture of `width` x `height` pixels of `depth` bits from
// `pixels`, refusing sizes no screen has and more than `capacity` bytes.
std::optional<vmdbg::Framebuffer> readFrame(const SpurMemory& memory, uint64_t pixels,
                                            int64_t width, int64_t height, int64_t depth,
                                            uint64_t capacity) {
    depth = std::abs(depth);
    if (pixels == 0 || width <= 0 || height <= 0 || width > 16384 || height > 16384
            || depth <= 0 || depth > 32)
        return std::nullopt;

    // Rows are padded to whole 32-bit words.
    const uint64_t rowBytes = (static_cast<uint64_t>(width) * depth + 31) / 32 * 4;
    const uint64_t size = rowBytes * static_cast<uint64_t>(height);
    if (size > capacity) return std::nullopt;

    vmdbg::Framebuffer frame;
    frame.width = static_cast<int>(width);
    frame.height = static_cast<int>(height);
    frame.depth = static_cast<int>(depth);
    frame.pixels.resize(size);
    if (!memory.readBytes(pixels, frame.pixels.data(), size)) return std::nullopt;
    return frame;
}

vmdbg::Region regionOf(const char* name, uint64_t start, uint64_t end, vmdbg::Region::Kind kind) {
    vmdbg::Region region;
    region.name = name;
    region.start = start;
    region.end = end;
    region.kind = kind;
    return region;
}

} // namespace

bool OSVMInspector::refreshRegions() {
    if (!image) return false;
    clearRegions();
    if (image->memoryReader()->isLive()) {
        addLiveRegions();
        return true;
    }
    addRegion(regionOf("oldSpace", image->objectMemoryBase(), image->objectMemoryEnd(),
                       vmdbg::Region::Kind::Objects));
    return true;
}

// A live VM keeps new space in front of old space, in one allocation that old
// space fills to its end.
void OSVMInspector::addLiveRegions() {
    const SpurMemory& memory = *image->memoryReader();
    const uint64_t oldStart = image->objectMemoryBase();
    const auto heapEnd = memory.allocationContaining(oldStart).second;
    const uint64_t newStart = memory.newSpaceStart();
    if (newStart != 0 && newStart < oldStart)
        addRegion(regionOf("newSpace", newStart, oldStart, vmdbg::Region::Kind::Objects));
    addRegion(regionOf("oldSpace", oldStart, heapEnd > oldStart ? heapEnd : image->objectMemoryEnd(),
                       vmdbg::Region::Kind::Objects));
    const auto [codeStart, codeEnd] = memory.allocationContaining(codeZoneAddress());
    if (codeEnd > codeStart) addRegion(regionOf("codeZone", codeStart, codeEnd, vmdbg::Region::Kind::Code));
}

// A VM without symbols is placed by the code of a frame it runs in machine code.
uint64_t OSVMInspector::codeZoneAddress() const {
    if (codeZone != 0) return codeZone;
    auto base = image->debugger()->getSymbolProvider()->getSymbolByName(SYM_METHOD_ZONE_BASE);
    if (base) return codeZone = image->memoryReader()->readWord(base->address);
    for (const CogFrame& frame : framesFrom(zone->activeFramePointer(), image))
        if (frame.isMachineCode()) return codeZone = frame.codeStart();
    return 0;
}

bool OSVMInspector::discoverClasses() {
    clearClasses();
    for (const auto& [name, cls] : image->classes()) {
        smalltalk::ClassEntry entry;
        entry.name = name;
        entry.oop = cls.oop;
        entry.superclassName = cls.superclassName;
        if (const auto* parent = image->findClass(cls.superclassName))
            entry.superclass = parent->oop;
        entry.category = cls.category;
        entry.package = cls.category;
        entry.instanceVariableNames = image->instanceVariableNamesOf(cls.oop);
        for (const auto& [variable, value] : image->classVariablesOf(cls.oop))
            entry.classVariableNames.push_back(variable);
        addClass(std::move(entry));
    }
    return !classes().empty();
}

std::vector<smalltalk::MethodEntry> OSVMInspector::methodsOf(
    const std::string& className) const {
    std::vector<smalltalk::MethodEntry> methods;
    for (const auto& method : image->methodsOf(image->resolveClassOop(className)))
        methods.push_back({method.selector, className, method.category,
                           method.oop});
    return methods;
}

std::string OSVMInspector::sourceOf(const smalltalk::MethodEntry& method) const {
    return image->sourceOfMethod(method.oop);
}

std::vector<smalltalk::Bytecode> OSVMInspector::bytecodesOf(const smalltalk::MethodEntry& method) const {
    return decodeMethod(image->methodAt(method.oop)).bytecodes;
}

std::vector<smalltalk::ClassVariable> OSVMInspector::classVariablesOf(
    const std::string& className) const {
    std::vector<smalltalk::ClassVariable> variables;
    for (const auto& [name, value] :
         image->classVariablesOf(image->resolveClassOop(className)))
        variables.push_back({name, value});
    return variables;
}

// The VM keeps where the screen's pixels are and their size in globals, set
// whenever the image installs its display and again when a collection moves
// it. They are read on every screen update, so a recording that touched little
// else of the VM still holds them. The Form is read when there are no symbols
// to find them by.
std::optional<vmdbg::Framebuffer> OSVMInspector::framebuffer() const {
    if (auto frame = framebufferFromVM()) return frame;
    return framebufferFromDisplay();
}

std::optional<vmdbg::Framebuffer> OSVMInspector::framebufferFromVM() const {
    SpurMemory* memory = image->memoryReader();
    smalldbg::Debugger* debugger = memory->debugger();
    auto* symbols = debugger ? debugger->getSymbolProvider() : nullptr;
    if (symbols == nullptr) return std::nullopt;
    auto bits = symbols->getSymbolByName(SYM_DISPLAY_BITS);
    auto width = symbols->getSymbolByName(SYM_DISPLAY_WIDTH);
    auto height = symbols->getSymbolByName(SYM_DISPLAY_HEIGHT);
    auto depth = symbols->getSymbolByName(SYM_DISPLAY_DEPTH);
    if (!bits || !width || !height || !depth) return std::nullopt;

    uint64_t pixels = 0;
    int32_t sizes[3] = {0, 0, 0};
    if (!memory->readBytes(bits->address, &pixels, sizeof(pixels))
            || !memory->readBytes(width->address, &sizes[0], sizeof(int32_t))
            || !memory->readBytes(height->address, &sizes[1], sizeof(int32_t))
            || !memory->readBytes(depth->address, &sizes[2], sizeof(int32_t)))
        return std::nullopt;
    return readFrame(*memory, pixels, sizes[0], sizes[1], sizes[2], UINT64_MAX);
}

// The image draws into the Form the special objects array holds as the
// display. A Form starts with the instance variables bits, width, height and
// depth; a negative depth means little-endian pixels, which changes nothing
// for a 32-bit pixel.
std::optional<vmdbg::Framebuffer> OSVMInspector::framebufferFromDisplay() const {
    const uint64_t display = image->specialObject(SpecialObject::THE_DISPLAY);
    if (display == 0 || Spur::isSmallInteger(display)) return std::nullopt;

    const uint64_t bits = image->slotOf(display, 0);
    const uint64_t width = image->slotOf(display, 1);
    const uint64_t height = image->slotOf(display, 2);
    const uint64_t depth = image->slotOf(display, 3);
    if (Spur::isSmallInteger(bits) || !Spur::isSmallInteger(width)
            || !Spur::isSmallInteger(height) || !Spur::isSmallInteger(depth))
        return std::nullopt;
    return readFrame(*image->memoryReader(), bits + Spur::BASE_HEADER_SIZE,
                     Spur::smallIntegerValue(width), Spur::smallIntegerValue(height),
                     Spur::smallIntegerValue(depth), image->numSlotsOf(bits) * 8);
}

} // namespace osvm
