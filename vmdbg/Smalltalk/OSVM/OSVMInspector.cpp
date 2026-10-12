#include "OSVMInspector.h"
#include "CogFrame.h"
#include <smalldbg/Debugger.h>
#include <smalldbg/symbols/SymbolProvider.h>

namespace osvm {

namespace {

// The global a Cog VM keeps the start of its machine code zone in.
constexpr const char* SYM_METHOD_ZONE_BASE = "methodZoneBase";

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
    const auto [heapStart, heapEnd] = memory.allocationContaining(oldStart);
    if (heapStart < oldStart) addRegion(regionOf("newSpace", heapStart, oldStart, vmdbg::Region::Kind::Objects));
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

std::vector<smalltalk::ClassVariable> OSVMInspector::classVariablesOf(
    const std::string& className) const {
    std::vector<smalltalk::ClassVariable> variables;
    for (const auto& [name, value] :
         image->classVariablesOf(image->resolveClassOop(className)))
        variables.push_back({name, value});
    return variables;
}

} // namespace osvm
