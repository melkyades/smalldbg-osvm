#include "OSVMDebugSession.h"
#include "OSVMInspectorJson.h"
#include "Json.h"
#include <smalldbg/Debugger.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>

using webside::Json;

namespace osvm {

namespace {

constexpr size_t MAX_CONTEXT_DEPTH = 512;
constexpr size_t MAX_HEAP_OBJECTS = 4000000;
constexpr uint64_t NEW_SPACE_CHUNK = 1u << 20;
// Class indices below this are the puns Spur uses for free chunks, forwarders
// and the like.
constexpr uint32_t FIRST_REAL_CLASS_INDEX = 32;
constexpr int MAX_PRIORITY = 100;

bool endsWithImage(const std::string& path) {
    static const std::string suffix = ".image";
    return path.size() > suffix.size() &&
           path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

OSVMDebugSession::OSVMDebugSession(smalldbg::Debuggee debuggee)
    : ownedDebugger(std::make_unique<smalldbg::Debugger>(
          smalldbg::Mode::External, nullptr, debuggee)) {}
OSVMDebugSession::~OSVMDebugSession() = default;

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

bool OSVMDebugSession::openImageFile(const std::string& path) {
    imageFile = std::make_unique<SpurImageFile>();
    if (!imageFile->open(path)) {
        lastError = imageFile->error();
        imageFile.reset();
        return false;
    }
    imagePath = path;

    image = std::make_unique<SpurImage>(imageFile.get());
    image->sourceFiles().locateFor(path);
    if (!image->load()) {
        lastError = image->error();
        image.reset();
        return false;
    }
    ownedInspector = std::make_unique<OSVMInspector>(image.get());
    return true;
}

bool OSVMDebugSession::openLiveVm(const std::string& vmImagePath) {
    processMemory = std::make_unique<SpurProcessMemory>(&debugger());

    uint64_t heapBase = 0;
    uint64_t heapEnd = 0;
    uint64_t specialObjects = 0;
    if (!vmImagePath.empty()) {
        SpurImageFile probe;
        if (probe.open(vmImagePath)) {
            heapBase = probe.baseAddress();
            heapEnd = probe.endAddress();
            specialObjects = probe.specialObjectsOop();
        }
    }

    if (!processMemory->locateRoots(heapBase, heapEnd, specialObjects)) {
        lastError = processMemory->error();
        processMemory.reset();
        return false;
    }

    image = std::make_unique<SpurImage>(processMemory.get());
    if (!vmImagePath.empty()) image->sourceFiles().locateFor(vmImagePath);
    if (!image->load()) {
        lastError = image->error();
        image.reset();
        return false;
    }
    ownedInspector = std::make_unique<OSVMInspector>(image.get());
    imagePath = vmImagePath;
    return true;
}

// How long to give the engine to bring the target to a stop. A stop normally
// lands in a few milliseconds; the cap is only there so a target that never
// stops says so rather than hanging.
constexpr int kSuspendWaitMs = 5000;

// An image file is a whole heap on disk: there is nothing to run and nothing
// for the engine to open, so it is read directly.
bool OSVMDebugSession::readsWithoutEngine(const vmdbg::TargetSpec& target) const {
    return target.kind == vmdbg::TargetKind::Launch && endsWithImage(target.path);
}

// However the VM was reached, what this needs from it is the same: the roots,
// read out of the live heap. A launch is the one that has to wait first, since
// the VM has not read its image in yet.
bool OSVMDebugSession::onTargetOpened(const vmdbg::TargetSpec& target) {
    const std::string vmImage = target.args.empty() ? imagePath : target.args.front();

    if (readsWithoutEngine(target)) return openImageFile(target.path);
    if (target.kind != vmdbg::TargetKind::Launch) return openLiveVm(vmImage);

    // Let the VM read its image in, then stop it so the heap is quiescent.
    // Suspending only asks: the engine raises the interrupt and answers before
    // the target has stopped, and until it has, reading through the debugger
    // fails. Everything below reads the heap, so wait for the stop to land.
    debugger().resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(
        startupPauseMs() < 0 ? kDefaultStartupPauseMs : startupPauseMs()));
    debugger().suspend();
    if (!debugger().isStopped())
        debugger().waitForEvent(smalldbg::StopReason::None, kSuspendWaitMs);
    if (!debugger().isStopped()) {
        lastError = "the VM did not stop when asked";
        return false;
    }

    if (openLiveVm(vmImage)) return true;

    // A stripped VM still leaves the image file browsable, but say why the live
    // heap was given up on. On its own the fallback reads as a choice rather
    // than a failure, and hides one that is usually fixable.
    const std::string liveError = lastError;
    if (!vmImage.empty() && openImageFile(vmImage)) {
        logOpening("live roots unavailable (" + liveError
                   + "), serving the image file");
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// run control
// ---------------------------------------------------------------------------

void OSVMDebugSession::describeHeader(Json& json, uint64_t oop) const {
    const SpurHeapObject object = image->objectAt(oop).asHeapObject();
    json.set("format", static_cast<int>(object.format()))
        .set("classIndex", static_cast<int>(object.classIndex()))
        .set("hash", static_cast<int>(object.identityHash()));
}

// ---------------------------------------------------------------------------
// green threads
// ---------------------------------------------------------------------------

std::vector<SpurContext> OSVMDebugSession::walkContexts(const SpurContext& context) const {
    std::vector<SpurContext> contexts;
    SpurContext current = context;
    while (current && contexts.size() < MAX_CONTEXT_DEPTH) {
        contexts.push_back(current);
        current = current.sender();
    }
    return contexts;
}

std::vector<SpurProcess> OSVMDebugSession::processesInHeap() const {
    std::vector<SpurProcess> found;

    const uint64_t processClass = image->resolveClassOop("Process");
    if (processClass == 0) return found;

    const auto isProcessClass = [&](SpurBehavior cls) {
        for (; cls; cls = cls.superclass())
            if (cls.oop() == processClass) return true;
        return false;
    };

    // A live VM allocates past the end of the image it loaded, up to the bridge
    // that ends its last segment.
    const uint64_t start = image->memoryReader()->baseAddress();
    const uint64_t limit = image->memoryReader()->isLive() ? UINT64_MAX
                                                           : image->memoryReader()->endAddress();
    SpurHeapObject object = image->heapObjectAt(start);
    size_t visited = 0;
    while (object.oop() >= start && object.oop() < limit && visited < MAX_HEAP_OBJECTS) {
        if (object.header() == 0) break;
        if (object.classIndex() == Spur::SEGMENT_BRIDGE && object.numSlots() == 0) break;
        const SpurHeapObject next = object.nextObject();
        if (next.oop() <= object.oop()) break;
        visited++;
        if (isProcessClass(SpurBehavior(object.spurClass()))) found.push_back(SpurProcess(object));
        object = next;
    }
    addProcessesInNewSpace(found, isProcessClass);
    return found;
}

// New space has no walkable end without the VM's globals, and holds dead
// objects past it, so it is searched for headers instead. A process created
// since the image started lives there until it survives enough scavenges. The
// VM reserves it in front of old space, in the same allocation.
void OSVMDebugSession::addProcessesInNewSpace(
    std::vector<SpurProcess>& found,
    const std::function<bool(SpurBehavior)>& isProcessClass) const {
    const uint64_t high = image->memoryReader()->baseAddress();
    const uint64_t low = image->memoryReader()->allocationContaining(high).first;
    std::map<uint32_t, bool> processIndices;
    std::vector<uint64_t> words(NEW_SPACE_CHUNK / sizeof(uint64_t));
    for (uint64_t chunk = low; chunk < high; chunk += NEW_SPACE_CHUNK) {
        const size_t count = static_cast<size_t>(
            std::min<uint64_t>(NEW_SPACE_CHUNK, high - chunk) / sizeof(uint64_t));
        if (!image->memoryReader()->readBytes(chunk, words.data(), count * sizeof(uint64_t)))
            continue;
        for (size_t i = 0; i < count; i++) {
            const uint64_t header = words[i];
            if (((header >> Spur::FORMAT_SHIFT) & Spur::FORMAT_MASK) != Fmt::FIXED) continue;
            const uint32_t classIndex = static_cast<uint32_t>(header & Spur::CLASS_INDEX_MASK);
            if (classIndex < FIRST_REAL_CLASS_INDEX) continue;
            auto known = processIndices.find(classIndex);
            if (known == processIndices.end()) {
                const uint64_t cls = image->classAtIndex(classIndex);
                known = processIndices.emplace(classIndex,
                    cls != 0 && isProcessClass(SpurBehavior(image->heapObjectAt(cls)))).first;
            }
            if (!known->second) continue;
            const SpurProcess process(image->heapObjectAt(chunk + i * sizeof(uint64_t)));
            if (looksLikeProcess(process)) found.push_back(process);
        }
    }
}

bool OSVMDebugSession::looksLikeProcess(const SpurProcess& process) const {
    const int priority = process.priority();
    if (priority < 1 || priority > MAX_PRIORITY) return false;
    const SpurContext context = process.suspendedContext();
    return !context || context.heapObject().format() == Fmt::FIXED_VARIABLE;
}


// Spur keeps activations in Context objects on the heap, so only the native
// frame of the interpreter tells which thread runs it.
std::shared_ptr<smalldbg::Thread> OSVMDebugSession::vmThread() const {
    return threadRunning("interpret");
}

bool OSVMDebugSession::readGreenThreads() {
    greenThreads.clear();
    if (!image) return false;

    const SpurProcessorScheduler scheduler = image->scheduler();
    if (!scheduler) return false;

    const auto alreadyKnown = [&](const SpurProcess& process) {
        for (const auto& thread : greenThreads)
            if (thread.process.oop() == process.oop()) return true;
        return false;
    };

    const auto addProcess = [&](const SpurProcess& process, bool active) {
        if (!process || alreadyKnown(process)) return;
        GreenThread thread;
        thread.process = process;
        thread.name = process.name();
        thread.priority = process.priority();
        if (thread.name.empty())
            thread.name = active ? "active process" : "process";
        if (active) thread.name += " (active)";
        thread.contexts = walkContexts(process.suspendedContext());
        greenThreads.push_back(thread);
    };

    addProcess(scheduler.activeProcess(), true);
    for (const auto& process : scheduler.quiescentProcesses()) addProcess(process, false);

    for (const auto& process : processesInHeap()) {
        if (process.suspendedContext()) addProcess(process, false);
    }
    return true;
}

int OSVMDebugSession::greenThreadCount() const {
    const_cast<OSVMDebugSession*>(this)->refreshGreenThreads();
    return static_cast<int>(greenThreads.size());
}

std::string OSVMDebugSession::getGreenThreadName(int threadIndex) const {
    if (threadIndex < 0 || threadIndex >= greenThreadCount()) return "";
    const GreenThread& thread = greenThreads[threadIndex];
    return thread.name + " [priority " + std::to_string(thread.priority) + "]";
}

std::string OSVMDebugSession::frameLabel(const SpurContext& context) const {
    const auto* location = image->locationOfMethod(context.method().oop());

    std::string label = location != nullptr
        ? location->className + ">>" + location->selector
        : "a CompiledMethod";

    if (context.isBlockContext()) label = "[] in " + label;
    return label;
}

std::string OSVMDebugSession::listSmalltalkFrames(int threadIndex) const {
    if (threadIndex < 0 || threadIndex >= greenThreadCount()) return "[]";

    auto array = Json::array();
    const auto& contexts = greenThreads[threadIndex].contexts;
    for (size_t i = 0; i < contexts.size(); i++) {
        const SpurContext& context = contexts[i];
        array.add(Json::object()
            .set("index", static_cast<int64_t>(i + 1))
            .set("type", "smalltalk")
            .set("language", "Smalltalk")
            .set("label", frameLabel(context))
            .set("receiver", context.receiver().printString())
            .set("oop", Json::hex(context.oop())));
    }
    return array.dump();
}

std::string OSVMDebugSession::getSmalltalkFrameDetail(int threadIndex, int frameIndex) const {
    if (threadIndex < 0 || threadIndex >= greenThreadCount()) return "{}";
    const auto& contexts = greenThreads[threadIndex].contexts;
    if (frameIndex < 1 || static_cast<size_t>(frameIndex) > contexts.size()) return "{}";

    const SpurContext& context = contexts[frameIndex - 1];
    const SpurCompiledMethod method = context.method();
    const auto* location = image->locationOfMethod(method.oop());

    auto json = Json::object()
        .set("index", frameIndex)
        .set("type", "smalltalk")
        .set("language", "Smalltalk")
        .set("label", frameLabel(context))
        .set("oop", Json::hex(context.oop()))
        .set("method", Json::hex(method.oop()))
        .set("source", method.sourceCode());

    if (location != nullptr) {
        json.set("class", location->className).set("selector", location->selector);
        const uint64_t classOop = image->resolveClassOop(location->className);
        json.set("category", image->categoryOfMethod(classOop, location->selector));
    }

    const SpurSmallInteger pc = context.programCounter().asSmallInteger();
    if (pc.isSmallInteger()) json.set("pc", pc.value());
    return json.dump();
}

std::string OSVMDebugSession::getSmalltalkFrameBindings(int threadIndex, int frameIndex) const {
    if (threadIndex < 0 || threadIndex >= greenThreadCount()) return "[]";
    const auto& contexts = greenThreads[threadIndex].contexts;
    if (frameIndex < 1 || static_cast<size_t>(frameIndex) > contexts.size()) return "[]";

    const SpurContext& context = contexts[frameIndex - 1];
    const uint32_t numArgs = context.method().header().numArgs;

    auto array = Json::array();
    const SpurObject receiver = context.receiver();
    array.add(Json::object()
        .set("name", "self")
        .set("value", receiver.printString())
        .set("raw", Json::hex(receiver.oop()))
        .set("type", "receiver"));

    const auto temporaries = context.temporaries();
    for (size_t i = 0; i < temporaries.size(); i++) {
        const SpurObject& value = temporaries[i];
        const bool isArgument = i < numArgs;
        const size_t ordinal = isArgument ? i + 1 : i - numArgs + 1;
        array.add(Json::object()
            .set("name", (isArgument ? "arg" : "tmp") + std::to_string(ordinal))
            .set("value", value.printString())
            .set("raw", Json::hex(value.oop()))
            .set("type", isArgument ? "argument" : "temporary"));
    }
    return array.dump();
}

std::string OSVMDebugSession::getSmalltalkStackContents(int threadIndex, int) const {
    if (threadIndex < 0 || threadIndex >= greenThreadCount()) return "[]";
    auto array = Json::array();
    for (const SpurContext& context : greenThreads[threadIndex].contexts) {
        array.add(Json::object()
            .set("oop", Json::hex(context.oop()))
            .set("label", frameLabel(context)));
    }
    return array.dump();
}

// ---------------------------------------------------------------------------
// class browsing
// ---------------------------------------------------------------------------

// The image holds each class's definition and comment as the browser that
// wrote them shows them.
Json OSVMDebugSession::classJson(const smalltalk::ClassEntry& entry) const {
    Json json = smalltalk::Session::classJson(entry);
    const uint64_t oop = image->resolveClassOop(entry.name);
    json.set("definition", image->definitionOfClass(oop));
    json.set("oop", addressHex(oop));
    if (!isMetaclassName(entry.name))
        json.set("comment", image->commentOfClass(oop));
    return json;
}

Json OSVMDebugSession::methodJson(const smalltalk::MethodEntry& method) const {
    Json json = smalltalk::Session::methodJson(method);
    const auto header = image->headerOfMethod(method.oop);
    json.set("numArgs", static_cast<int64_t>(header.numArgs));
    json.set("numTemps", static_cast<int64_t>(header.numTemps));
    return json;
}

// An image opened off disk has no process behind it, so it is neither
// stopped nor running.
std::string OSVMDebugSession::getStopReason() const {
    if (imageFile) return "image";
    return smalltalk::Session::getStopReason();
}

} // namespace osvm
