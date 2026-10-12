#pragma once

#include "VMInspector.h"
#include "CogStackZone.h"
#include "SpurImage.h"
#include "Smalltalk/Inspector.h"
#include <memory>
#include <string>

namespace osvm {

/** An OSVMInspector serves the /inspect and /regions routes for an OpenSmalltalk heap. **/
class OSVMInspector : public smalltalk::Inspector {
public:
    explicit OSVMInspector(SpurImage* img)
        : image(img),
          zone(img->memoryReader()->isLive() ? std::make_unique<CogStackZone>(img) : nullptr) {}

    // ---- the shared inspector's view of this VM ----------------------
    std::unique_ptr<smalltalk::Object> objectFor(uint64_t oop) const override {
        return std::make_unique<SpurObject>(image->objectAt(oop));
    }
    std::unique_ptr<smalltalk::HeapObject> heapObjectFor(uint64_t address) const override {
        return std::make_unique<SpurHeapObject>(image->heapObjectAt(address));
    }
    /** Records old space and, in a live VM, the new space in front of it. **/
    bool refreshRegions() override;
    /** Fills the class table from the classes the image indexed as it
     *  loaded. **/
    bool discoverClasses() override;
    std::vector<smalltalk::MethodEntry> methodsOf(
        const std::string& className) const override;
    std::string sourceOf(const smalltalk::MethodEntry& method) const override;
    std::vector<smalltalk::ClassVariable> classVariablesOf(
        const std::string& className) const override;

    /** Returns where a live Cog VM runs its processes, or null for an image file. It
     *  lives as long as the inspector. **/
    CogStackZone* stackZone() const { return zone.get(); }

private:
    void addLiveRegions();

    SpurImage* image{nullptr};
    std::unique_ptr<CogStackZone> zone;

public:
    /** Returns the image this inspector reads through. **/
    SpurImage* imageHandle() const { return image; }
};

} // namespace osvm
