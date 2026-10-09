#pragma once

#include "WebsideSession.h"
#include "Smalltalk/OSVM/OSVMInspector.h"
#include "Smalltalk/Session.h"
#include "Smalltalk/OSVM/SpurImage.h"
#include "Smalltalk/OSVM/SpurImageFile.h"
#include "Smalltalk/OSVM/SpurProcessMemory.h"
#include <memory>
#include <string>
#include <vector>

namespace smalldbg { class Debugger; }

namespace osvm {

/// A Webside session over an OpenSmalltalk image.
///
/// Opens either a .image file directly or a VM process running one. Class
/// browsing is identical in both modes because it goes through SpurImage;
/// only run control and green threads need a live target.
/// How long a launched VM is left to read its image in, when nobody says.
static constexpr int kDefaultStartupPauseMs = 3000;

class OSVMDebugSession : public smalltalk::Session {
public:
    explicit OSVMDebugSession(smalldbg::Debuggee debuggee);
    ~OSVMDebugSession() override;

    /// The image the VM was started on may be named after the "--". It is
    /// optional, and only a hint: the heap is found by scanning either way,
    /// but old space keeps its layout across a load, so the image is what says
    /// where the special objects array sits inside it.
    bool onTargetOpened(const vmdbg::TargetSpec& target) override;
    bool readsWithoutEngine(const vmdbg::TargetSpec& target) const override;
    const char* dialectTag() const override { return "osvm"; }

    bool isActive() const override;
    std::optional<int> getPid() const override;

    // ---- run control (live targets only) ----

    std::string getStopReason() const override;
    std::string getRegisters() const override;

    // ---- green threads (Smalltalk processes) ----
    std::shared_ptr<smalldbg::Thread> vmThread() const override;
    void refreshGreenThreads() override;
    int greenThreadCount() const override;
    std::string getGreenThreadName(int threadIndex) const override;
    std::string listSmalltalkFrames(int threadIndex) const override;
    std::string getSmalltalkFrameDetail(int threadIndex, int frameIndex) const override;
    std::string getSmalltalkFrameBindings(int threadIndex, int frameIndex) const override;
    std::string getSmalltalkStackContents(int threadIndex, int selectedFrame) const override;

    std::string listBreakpoints() const override;
    std::string readMemory(uint64_t address, size_t size) const override;
    std::string waitForEvent(int timeoutMs = 5000) override;
    std::string describeObject(uint64_t handle, size_t maxDepth = 1) const override;

    smalldbg::Debugger& debugger() const override { return *ownedDebugger; }
    OSVMInspector& inspector() const override { return *ownedInspector; }

    SpurImage* getImage() const { return image.get(); }
    const std::string& error() const { return lastError; }

    /// How long to let a freshly launched VM run before suspending it. The
    /// heap does not exist until the VM has read its image in.

protected:
    smalltalk::Inspector& smalltalkInspector() const override { return *ownedInspector; }
    webside::Json classJson(const smalltalk::ClassEntry& entry) const override;
    webside::Json methodJson(const smalltalk::MethodEntry& method) const override;

private:
    bool openImageFile(const std::string& path);
    bool openLiveVm(const std::string& imagePath);

    /// One Smalltalk Process reified as a debugger-visible thread.
    struct GreenThread {
        SpurProcess process;
        std::string name;
        int priority{0};
        std::vector<SpurContext> contexts;
    };
    std::vector<SpurContext> walkContexts(const SpurContext& context) const;
    std::string frameLabel(const SpurContext& context) const;
    /// Processes blocked on a semaphore are on that semaphore's list, not on
    /// the scheduler's, so the heap is the only place that sees all of them.
    std::vector<SpurProcess> processesInHeap() const;

    // An image-only session never attaches this, but it is always there, so
    // isActive() rather than a null test tells a live target from a file.
    const std::unique_ptr<smalldbg::Debugger> ownedDebugger;
    std::unique_ptr<SpurImageFile> imageFile;
    std::unique_ptr<SpurProcessMemory> processMemory;
    std::unique_ptr<SpurImage> image;
    // Needs the image, so it is built while opening rather than in the
    // constructor. The workspace hands the session out only once it is set.
    std::unique_ptr<OSVMInspector> ownedInspector;
    std::vector<GreenThread> greenThreads;
    std::string lastError;
    std::string imagePath;
};

} // namespace osvm
