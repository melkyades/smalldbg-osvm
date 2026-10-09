#pragma once

#include "VMInspector.h"
#include "SpurImage.h"
#include "Smalltalk/Inspector.h"
#include <memory>
#include <string>

namespace osvm {

/// Serves the /inspect and /regions routes for an OpenSmalltalk heap.
class OSVMInspector : public smalltalk::Inspector {
public:
    explicit OSVMInspector(SpurImage* img)
        : image(img) {}

    StackRegion evaluatorStack() const override;

    // ---- the shared inspector's view of this VM ----------------------
    std::unique_ptr<smalltalk::Object> objectFor(uint64_t oop) const override {
        return std::make_unique<SpurObject>(image->objectAt(oop));
    }
    std::unique_ptr<smalltalk::HeapObject> heapObjectFor(uint64_t address) const override {
        return std::make_unique<SpurHeapObject>(image->heapObjectAt(address));
    }
    /// An image is one contiguous old space with fixed bounds.
    bool refreshRegions() override;
    /// The image indexes every class as it loads; there is nothing to walk.
    bool discoverClasses() override { return true; }

    /// JSON describing the heap extent and the well-known roots.
    /// JSON classifying an address as heap, immediate, or outside the image.

private:
    SpurImage* image{nullptr};

public:
    /// The image these reads go through.
    SpurImage* imageHandle() const { return image; }
};

} // namespace osvm
