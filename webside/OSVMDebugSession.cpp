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
constexpr size_t MAX_STACK_SLOTS = 8192;
// A BlockClosure's outerContext comes first, then the pc its body starts at.
constexpr uint64_t BLOCK_START_PC = 1;
// The JIT's first mapped instruction follows the method's header and a short
// entry sequence.
constexpr uint64_t MAX_MAPPED_CODE_OFFSET = 0x400;

// Sets what identifies an activation: its Context, and for a frame its address,
// which is all a frame the VM never made a Context for has.
void setIdentity(Json& json, const SpurActivation& activation) {
    if (activation.context()) json.set("oop", Json::hex(activation.context().oop()));
    if (activation.isFrame()) json.set("framePointer", Json::hex(activation.frame().framePointer()));
}

// Returns the frame the activation at `index` called, when it is on the same
// stack page, or null. A base frame returns through the VM, so it does not say
// where its caller is. The frame lives as long as `activations`.
const CogFrame* calleeOnPage(const std::vector<SpurActivation>& activations, size_t index) {
    if (index == 0 || !activations[index - 1].isFrame()) return nullptr;
    const CogFrame& callee = activations[index - 1].frame();
    return callee.isBase() ? nullptr : &callee;
}

// Sets where a machine code frame's code starts, and where in it the frame is
// when that is known.
void setMachineCode(Json& json, const std::vector<SpurActivation>& activations, size_t index) {
    const SpurActivation& activation = activations[index];
    if (!activation.isFrame() || !activation.frame().isMachineCode()) return;
    const CogFrame& frame = activation.frame();
    json.set("functionAddress", Json::hex(frame.codeStart()));
    if (uint64_t ip = frame.instructionPointer(calleeOnPage(activations, index)))
        json.set("ip", Json::hex(ip));
}

// Returns where the body of the block an activation runs starts, which its
// closure knows, or 0 for a method activation and for a block without one.
int blockStartOf(const SpurActivation& activation) {
    if (!activation.isBlock()) return 0;
    const SpurObject closure = activation.isFrame() ? activation.frame().closure()
                                                    : activation.context().closure();
    if (closure.isImmediate() || closure.isNil()) return 0;
    const SpurObject start = closure.asHeapObject().slotAt(BLOCK_START_PC);
    return start.isSmallInteger() ? static_cast<int>(start.asSmallInteger().value()) : 0;
}

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

uint64_t OSVMDebugSession::runningFramePointer() const {
    CogStackZone* zone = ownedInspector->stackZone();
    if (zone == nullptr || !debugger().isStopped()) return 0;
    if (framePointerVersion != debugger().stateVersion()) {
        framePointerVersion = debugger().stateVersion();
        framePointer = zone->activeFramePointer();
    }
    return framePointer;
}

// A VM with symbols names its interpreter. A JIT runs Smalltalk outside any
// function, but allocates its stack zone on the stack of the thread running it.
std::shared_ptr<smalldbg::Thread> OSVMDebugSession::vmThread() const {
    if (auto thread = threadRunning("interpret")) return thread;
    CogStackZone* zone = ownedInspector->stackZone();
    return zone != nullptr ? zone->threadHolding(runningFramePointer()) : nullptr;
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
        thread.active = active;
        // A running process's context is the VM's current frame, not one it saved.
        const uint64_t framePointer = active && !process.suspendedContext() ? runningFramePointer() : 0;
        thread.activations = activationsFrom(process.suspendedContext(), framePointer,
                                             image.get(), MAX_CONTEXT_DEPTH);
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

int OSVMDebugSession::activeGreenThread() const {
    const int count = greenThreadCount();
    for (int i = 0; i < count; i++)
        if (greenThreads[i].active) return i;
    return -1;
}

std::string OSVMDebugSession::greenThreadOn(uint64_t nativeThreadId) const {
    auto thread = vmThread();
    const int active = activeGreenThread();
    if (!thread || thread->getThreadId() != nativeThreadId || active < 0) return {};
    return greenThreads[active].name;
}

const SpurMethodMap& OSVMDebugSession::methodMap(const SpurCompiledMethod& method) const {
    if (methodMapsVersion != debugger().stateVersion()) {
        methodMapsVersion = debugger().stateVersion();
        methodMaps.clear();
    }
    auto& map = methodMaps[method.oop()];
    if (!map) map = std::make_unique<SpurMethodMap>(method);
    return *map;
}

std::optional<uint64_t> OSVMDebugSession::mappedCodeOffset() const {
    if (codeOffsetVersion == debugger().stateVersion()) return codeOffset;
    codeOffsetVersion = debugger().stateVersion();
    std::map<uint64_t, int> fits;
    const int count = greenThreadCount();
    for (int t = 0; t < count; t++) {
        const auto& activations = greenThreads[t].activations;
        for (size_t i = 0; i < activations.size(); i++) {
            if (!activations[i].isFrame() || !activations[i].frame().isMachineCode()) continue;
            const CogFrame& frame = activations[i].frame();
            const uint64_t ip = frame.instructionPointer(calleeOnPage(activations, i));
            if (ip == 0) continue;
            for (uint64_t offset : frame.sendReturnOffsets()) {
                const uint64_t fit = ip - frame.methodCode() - offset;
                if (fit < MAX_MAPPED_CODE_OFFSET) fits[fit]++;
            }
        }
    }
    codeOffset.reset();
    int best = 0;
    for (const auto& [offset, frames] : fits)
        if (frames > best) { best = frames; codeOffset = offset; }
    return codeOffset;
}

// An interpreter frame and a Context are at the bytecode their pc falls in; a
// machine code frame at the send it returns from.
std::optional<size_t> OSVMDebugSession::bytecodeIndexOf(
    const std::vector<SpurActivation>& activations, size_t index) const {
    const SpurActivation& activation = activations[index];
    const SpurMethodMap& map = methodMap(activation.method());
    if (!activation.isFrame()) {
        const SpurObject pc = activation.context().programCounter();
        if (!pc.isSmallInteger()) return std::nullopt;
        return map.bytecodeAt(static_cast<int>(pc.asSmallInteger().value()) - 1);
    }
    const CogFrame& frame = activation.frame();
    const uint64_t ip = frame.instructionPointer(calleeOnPage(activations, index));
    if (ip == 0) return std::nullopt;
    if (!frame.isMachineCode()) {
        const uint64_t bytes = frame.method().oop() + Spur::BASE_HEADER_SIZE;
        return map.bytecodeAt(static_cast<int>(ip - bytes) + 1);
    }
    const auto offset = mappedCodeOffset();
    if (!offset) return std::nullopt;
    const std::vector<uint64_t> sends = frame.sendReturnOffsets();
    for (size_t k = 0; k < sends.size(); k++)
        if (frame.methodCode() + *offset + sends[k] == ip) return map.compiledSend(k, sends.size());
    return std::nullopt;
}

std::optional<int> OSVMDebugSession::nextPCOf(const std::vector<SpurActivation>& activations,
                                              size_t index) const {
    const SpurActivation& activation = activations[index];
    if (!activation.isFrame()) {
        const SpurObject pc = activation.context().programCounter();
        if (pc.isNil()) return 0;
        if (!pc.isSmallInteger()) return std::nullopt;
        return static_cast<int>(pc.asSmallInteger().value());
    }
    const auto bytecode = bytecodeIndexOf(activations, index);
    if (!bytecode) return std::nullopt;
    const auto& found = methodMap(activation.method()).bytecodes()[*bytecode];
    return found.pc + static_cast<int>(found.bytes.size());
}

// Applies the image's own Process>>isTerminated: a process that is not running
// is terminated when none of its activations has code left to run. A process
// whose place is not known counts as live.
bool OSVMDebugSession::isGreenThreadLive(int threadIndex) const {
    if (threadIndex < 0 || threadIndex >= greenThreadCount()) return false;
    const GreenThread& thread = greenThreads[threadIndex];
    if (thread.active) return true;
    const auto& activations = thread.activations;
    for (size_t i = 0; i < activations.size(); i++) {
        const auto pc = nextPCOf(activations, i);
        if (!pc) return true;
        if (*pc == 0) continue;
        const int blockStart = blockStartOf(activations[i]);
        if (!methodMap(activations[i].method()).isAtEnd(*pc, blockStart)) return true;
    }
    return false;
}

std::vector<std::string> OSVMDebugSession::temporaryNames(const SpurActivation& activation) const {
    const SpurMethodMap& map = methodMap(activation.method());
    if (const int start = blockStartOf(activation)) return map.blockNames(start);
    return map.methodNames(activation.method().header().numTemps);
}

std::string OSVMDebugSession::frameLabel(const SpurActivation& activation) const {
    const auto* location = image->locationOfMethod(activation.method().oop());

    std::string label = location != nullptr
        ? location->className + ">>" + location->selector
        : "a CompiledMethod";

    if (activation.isBlock()) label = "[] in " + label;
    return label;
}

std::string OSVMDebugSession::listSmalltalkFrames(int threadIndex) const {
    if (threadIndex < 0 || threadIndex >= greenThreadCount()) return "[]";

    auto array = Json::array();
    const auto& activations = greenThreads[threadIndex].activations;
    for (size_t i = 0; i < activations.size(); i++) {
        const SpurActivation& activation = activations[i];
        Json frame = Json::object()
            .set("index", static_cast<int64_t>(i + 1))
            .set("type", "smalltalk")
            .set("language", "Smalltalk")
            .set("label", frameLabel(activation))
            .set("receiver", activation.receiver().printString());
        setIdentity(frame, activation);
        setMachineCode(frame, activations, i);
        array.add(std::move(frame));
    }
    return array.dump();
}

std::string OSVMDebugSession::getSmalltalkFrameDetail(int threadIndex, int frameIndex) const {
    if (threadIndex < 0 || threadIndex >= greenThreadCount()) return "{}";
    const auto& activations = greenThreads[threadIndex].activations;
    if (frameIndex < 1 || static_cast<size_t>(frameIndex) > activations.size()) return "{}";

    const SpurActivation& activation = activations[frameIndex - 1];
    const SpurCompiledMethod method = activation.method();
    const auto* location = image->locationOfMethod(method.oop());

    smalltalk::MethodEntry entry;
    entry.oop = method.oop();
    if (location != nullptr) {
        entry.selector = location->selector;
        entry.className = location->className;
        entry.category = image->categoryOfMethod(image->resolveClassOop(location->className),
                                                 location->selector);
    }

    auto json = Json::object()
        .set("index", frameIndex)
        .set("type", "smalltalk")
        .set("language", "Smalltalk")
        .set("label", frameLabel(activation))
        .set("class", Json::object().set("name", entry.className))
        .set("method", methodJson(entry));
    setIdentity(json, activation);
    setMachineCode(json, activations, static_cast<size_t>(frameIndex - 1));

    const SpurMethodMap& map = methodMap(method);
    if (auto index = bytecodeIndexOf(activations, static_cast<size_t>(frameIndex - 1))) {
        smalltalk::FramePosition position;
        position.bytecodePC = map.bytecodes()[*index].pc;
        if (auto send = map.sourceOf(*index)) position.source = std::make_pair(send->start, send->end);
        setFramePosition(json, position, json["method"]["source"].asString());
    }
    return json.dump();
}

std::string OSVMDebugSession::getSmalltalkFrameBindings(int threadIndex, int frameIndex) const {
    if (threadIndex < 0 || threadIndex >= greenThreadCount()) return "[]";
    const auto& activations = greenThreads[threadIndex].activations;
    if (frameIndex < 1 || static_cast<size_t>(frameIndex) > activations.size()) return "[]";

    const SpurActivation& activation = activations[frameIndex - 1];
    const uint32_t numArgs = activation.isFrame() ? activation.frame().numArgs()
                                                  : activation.method().header().numArgs;
    const std::vector<std::string> names = temporaryNames(activation);

    auto array = Json::array();
    const SpurObject receiver = activation.receiver();
    array.add(Json::object()
        .set("name", "self")
        .set("value", receiver.printString())
        .set("raw", Json::hex(receiver.oop()))
        .set("type", "receiver"));

    const auto temporaries = activation.temporaries();
    for (size_t i = 0; i < temporaries.size(); i++) {
        const SpurObject& value = temporaries[i];
        const bool isArgument = i < numArgs;
        const size_t ordinal = isArgument ? i + 1 : i - numArgs + 1;
        array.add(Json::object()
            .set("name", i < names.size() ? names[i]
                                          : (isArgument ? "arg" : "tmp") + std::to_string(ordinal))
            .set("value", value.printString())
            .set("raw", Json::hex(value.oop()))
            .set("type", isArgument ? "argument" : "temporary"));
    }
    return array.dump();
}

// The slots of the frames on the VM's stack pages, each frame owning what it
// pushed for the call it is making. Activations that are only Contexts on the
// heap have no stack to show.
std::string OSVMDebugSession::getSmalltalkStackContents(int threadIndex, int selectedFrame) const {
    if (threadIndex < 0 || threadIndex >= greenThreadCount()) return "[]";
    const auto& activations = greenThreads[threadIndex].activations;
    auto array = Json::array();
    const CogFrame* callee = nullptr;
    for (size_t i = 0; i < activations.size(); i++) {
        if (!activations[i].isFrame()) { callee = nullptr; continue; }
        const CogFrame& frame = activations[i].frame();
        const int index = static_cast<int>(i + 1);
        const bool calleeOnPage = callee != nullptr && !callee->isBase();
        const uint64_t low = calleeOnPage ? callee->highestAddress() + Spur::BYTES_PER_OOP
                                          : frame.lowestAddress();
        for (uint64_t address = low; address <= frame.highestAddress() &&
                                     array.size() < MAX_STACK_SLOTS;
             address += Spur::BYTES_PER_OOP) {
            const uint64_t value = image->memoryReader()->readWord(address);
            Json entry = Json::object()
                .set("address", Json::hex(address))
                .set("size", static_cast<int64_t>(Spur::BYTES_PER_OOP))
                .set("value", Json::hex(value))
                .set("frameIndex", index)
                .set("frameRel", index < selectedFrame ? -1 : index == selectedFrame ? 0 : 1);
            const std::string role = frame.roleOf(address);
            if (!role.empty()) entry.set("role", role);
            if (frame.holdsObject(address)) entry.set("printString", image->printStringOf(value));
            array.add(std::move(entry));
        }
        callee = &frame;
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
