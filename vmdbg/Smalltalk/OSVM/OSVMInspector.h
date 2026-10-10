#pragma once

#include "VMInspector.h"
#include "SpurImage.h"
#include "Smalltalk/Inspector.h"
#include <memory>
#include <string>

namespace osvm {

/** An OSVMInspector serves the /inspect and /regions routes for an OpenSmalltalk heap. **/
class OSVMInspector : public smalltalk::Inspector {
public:
    explicit OSVMInspector(SpurImage* img)
        : image(img) {}

    // ---- the shared inspector's view of this VM ----------------------
    std::unique_ptr<smalltalk::Object> objectFor(uint64_t oop) const override {
        return std::make_unique<SpurObject>(image->objectAt(oop));
    }
    std::unique_ptr<smalltalk::HeapObject> heapObjectFor(uint64_t address) const override {
        return std::make_unique<SpurHeapObject>(image->heapObjectAt(address));
    }
    /** Records the image's old space as the only region, since it is one contiguous
     *  range with fixed bounds. **/
    bool refreshRegions() override;
    /** Fills the class table from the classes the image indexed as it
     *  loaded. **/
    bool discoverClasses() override;
    std::vector<smalltalk::MethodEntry> methodsOf(
        const std::string& className) const override;
    std::string sourceOf(const smalltalk::MethodEntry& method) const override;
    std::vector<smalltalk::ClassVariable> classVariablesOf(
        const std::string& className) const override;

private:
    SpurImage* image{nullptr};

public:
    /** Returns the image this inspector reads through. **/
    SpurImage* imageHandle() const { return image; }
};

} // namespace osvm
