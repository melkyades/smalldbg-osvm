#pragma once

#include "WebsideSession.h"
#include "Smalltalk/OSVM/OSVMInspector.h"
#include "Smalltalk/Session.h"
#include "Smalltalk/OSVM/SpurImage.h"
#include "Smalltalk/OSVM/SpurImageFile.h"
#include "Smalltalk/OSVM/SpurProcessMemory.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace smalldbg { class Debugger; }

namespace osvm {

/** kDefaultStartupPauseMs is how long a launched VM runs before it is suspended, unless
 *  the session is told otherwise. The heap does not exist until the VM has read its image in. **/
static constexpr int kDefaultStartupPauseMs = 3000;

/** An OSVMDebugSession is a Webside session over an OpenSmalltalk image, read either from a
 *  .image file or from a VM process running it. Class browsing reads through SpurImage the
 *  same way in both cases; only run control and green threads need a live VM. **/
class OSVMDebugSession : public smalltalk::Session {
public:
    explicit OSVMDebugSession(smalldbg::Debuggee debuggee);
    ~OSVMDebugSession() override;

    /** Opens an image file directly, or reads the heap of a VM, letting a launched VM read its
     *  image in first. The image a VM was started on, optionally named after the "--", is a
     *  hint that says where the special objects array sits, since old space keeps its layout
     *  across a load. **/
    bool onTargetOpened(const vmdbg::TargetSpec& target) override;
    bool readsWithoutEngine(const vmdbg::TargetSpec& target) const override;
    const char* dialectTag() const override { return "osvm"; }

    // ---- run control (live targets only) ----

    std::string getStopReason() const override;

    // ---- green threads (Smalltalk processes) ----
    std::shared_ptr<smalldbg::Thread> vmThread() const override;
    bool readGreenThreads() override;
    int greenThreadCount() const override;
    std::string getGreenThreadName(int threadIndex) const override;
    std::string listSmalltalkFrames(int threadIndex) const override;
    std::string getSmalltalkFrameDetail(int threadIndex, int frameIndex) const override;
    std::string getSmalltalkFrameBindings(int threadIndex, int frameIndex) const override;
    std::string getSmalltalkStackContents(int threadIndex, int selectedFrame) const override;

    /** Returns the debugger the heap is read through: the one over the image
     *  file, or the one attached to the VM. **/
    smalldbg::Debugger& debugger() const override {
        return imageFile ? *imageFile->debugger() : *ownedDebugger;
    }
    OSVMInspector& inspector() const override { return *ownedInspector; }

    SpurImage* getImage() const { return image.get(); }
    const std::string& error() const { return lastError; }

protected:
    smalltalk::Inspector& smalltalkInspector() const override { return *ownedInspector; }
    webside::Json classJson(const smalltalk::ClassEntry& entry) const override;
    webside::Json methodJson(const smalltalk::MethodEntry& method) const override;
    void describeHeader(webside::Json& json, uint64_t oop) const override;

private:
    bool openImageFile(const std::string& path);
    bool openLiveVm(const std::string& imagePath);

    /** A GreenThread shows one Smalltalk Process to the debugger as a thread. **/
    struct GreenThread {
        SpurProcess process;
        std::string name;
        int priority{0};
        std::vector<SpurContext> contexts;
    };
    std::vector<SpurContext> walkContexts(const SpurContext& context) const;
    std::string frameLabel(const SpurContext& context) const;
    /** Returns every Process found by walking the heap, since processes blocked on a
     *  semaphore are on that semaphore's list rather than the scheduler's. **/
    std::vector<SpurProcess> processesInHeap() const;
    void addProcessesInNewSpace(std::vector<SpurProcess>& found,
                                const std::function<bool(SpurBehavior)>& isProcessClass) const;
    bool looksLikeProcess(const SpurProcess& process) const;

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
