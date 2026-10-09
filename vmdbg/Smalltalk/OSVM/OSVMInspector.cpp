#include "OSVMInspector.h"


namespace osvm {

bool OSVMInspector::refreshRegions() {
    if (!image) return false;
    clearRegions();
    vmdbg::Region oldSpace;
    oldSpace.name = "oldSpace";
    oldSpace.start = image->objectMemoryBase();
    oldSpace.end = image->objectMemoryEnd();
    oldSpace.isObjectMemory = true;
    addRegion(std::move(oldSpace));
    return true;
}


vmdbg::VMInspector::StackRegion OSVMInspector::evaluatorStack() const {
    // Spur keeps activations in Context objects on the heap rather than in a
    // contiguous evaluator stack, so there is no region to report.
    return StackRegion{};
}




} // namespace osvm
