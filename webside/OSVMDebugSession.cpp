#include "OSVMDebugSession.h"
#include "OSVMInspectorJson.h"
#include "Json.h"
#include <smalldbg/Debugger.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <thread>

using webside::Json;

namespace osvm {

namespace {

constexpr size_t MAX_CONTEXT_DEPTH = 512;
constexpr size_t MAX_HEAP_OBJECTS = 4000000;

std::string lowercased(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool matches(const std::string& candidate, const std::string& text,
             bool ignoreCase, const std::string& condition) {
    const std::string a = ignoreCase ? lowercased(candidate) : candidate;
    const std::string b = ignoreCase ? lowercased(text) : text;
    if (b.empty()) return false;
    if (condition == "beginning") return a.rfind(b, 0) == 0;
    if (condition == "ending") return a.size() >= b.size() &&
                                     a.compare(a.size() - b.size(), b.size(), b) == 0;
    if (condition == "exact") return a == b;
    return a.find(b) != std::string::npos;
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

bool OSVMDebugSession::isActive() const { return image != nullptr; }

std::optional<int> OSVMDebugSession::getPid() const {
    auto pid = debugger().attachedPid();
    if (!pid) return std::nullopt;
    return static_cast<int>(*pid);
}

// ---------------------------------------------------------------------------
// run control
// ---------------------------------------------------------------------------

std::string OSVMDebugSession::getRegisters() const { return "{}"; }

std::string OSVMDebugSession::listBreakpoints() const { return "[]"; }

std::string OSVMDebugSession::readMemory(uint64_t address, size_t size) const {
    if (!image) return "{}";
    std::string bytes(size, '\0');
    if (!image->memoryReader()->readBytes(address, bytes.data(), size))
        return Json::object().set("error", "unreadable address").dump();

    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(size * 2);
    for (unsigned char c : bytes) {
        hex += digits[c >> 4];
        hex += digits[c & 0xF];
    }
    return Json::object()
        .set("address", Json::hex(address))
        .set("size", static_cast<int64_t>(size))
        .set("data", hex)
        .dump();
}

std::string OSVMDebugSession::waitForEvent(int) { return "{}"; }

std::string OSVMDebugSession::describeObject(uint64_t handle, size_t) const {
    return webside::inspectOSVMObject(inspector(), handle, 64);
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

    const uint64_t start = image->memoryReader()->baseAddress();
    const uint64_t limit = image->memoryReader()->endAddress();
    SpurHeapObject object = image->heapObjectAt(start);
    size_t visited = 0;
    while (object.oop() >= start && object.oop() < limit && visited < MAX_HEAP_OBJECTS) {
        const SpurHeapObject next = object.nextObject();
        if (next.oop() <= object.oop()) break;
        visited++;
        if (isProcessClass(SpurBehavior(object.spurClass()))) found.push_back(SpurProcess(object));
        object = next;
    }
    return found;
}

// Spur keeps activations in Context objects on the heap, so only the native
// frame of the interpreter tells which thread runs it.
std::shared_ptr<smalldbg::Thread> OSVMDebugSession::vmThread() const {
    return threadRunning("interpret");
}

void OSVMDebugSession::refreshGreenThreads() {
    greenThreads.clear();
    if (!image) return;

    const SpurProcessorScheduler scheduler = image->scheduler();
    if (!scheduler) return;

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
}

int OSVMDebugSession::greenThreadCount() const {
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

bool OSVMDebugSession::discoverClasses() { return image != nullptr; }

std::string OSVMDebugSession::classJson(const SpurImage::ClassEntry& entry, bool meta) const {
    const uint64_t oop = meta ? entry.metaclass : entry.oop;
    const std::string name = meta ? entry.name + " class" : entry.name;

    auto json = Json::object()
        .set("name", name)
        .set("definition", image->definitionOfClass(oop))
        .set("comment", meta ? std::string() : image->commentOfClass(entry.oop))
        .set("category", entry.category)
        .set("package", entry.category)
        .set("variable", false)
        .set("printString", name)
        .set("oop", Json::hex(oop));

    const std::string superclass = image->superclassNameOf(oop);
    if (superclass.empty()) json.set("superclass", nullptr);
    else json.set("superclass", superclass);

    json.set("class", meta ? std::string("Metaclass") : name + " class");
    return json.dump();
}

std::string OSVMDebugSession::listClasses(const std::string& root, bool namesOnly,
                                          bool, int) const {
    if (!image) return "[]";
    auto array = Json::array();

    const auto include = [&](const SpurImage::ClassEntry& entry) {
        if (namesOnly) {
            array.add(entry.name);
            return;
        }
        array.add(Json::parse(classJson(entry, false)));
    };

    if (root.empty()) {
        for (const auto& [name, entry] : image->classes()) include(entry);
        return array.dump();
    }

    // Walk the subclass tree beneath `root`.
    std::vector<std::string> pending{root};
    while (!pending.empty()) {
        const std::string current = pending.back();
        pending.pop_back();
        const auto* entry = image->findClass(current);
        if (entry == nullptr) continue;
        include(*entry);
        for (const auto& child : image->subclassNamesOf(entry->oop))
            pending.push_back(child);
    }
    return array.dump();
}

std::string OSVMDebugSession::getClass(const std::string& name) const {
    if (!image) return "{}";
    static const std::string suffix = " class";
    const bool meta = name.size() > suffix.size() &&
                      name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    const std::string base = meta ? name.substr(0, name.size() - suffix.size()) : name;

    const auto* entry = image->findClass(base);
    if (entry == nullptr) return "{}";
    return classJson(*entry, meta);
}

std::string OSVMDebugSession::getSubclasses(const std::string& name) const {
    if (!image) return "[]";
    const uint64_t classOop = image->resolveClassOop(name);
    if (classOop == 0) return "[]";

    auto array = Json::array();
    for (const auto& child : image->subclassNamesOf(classOop)) {
        const auto* entry = image->findClass(child);
        if (entry != nullptr) array.add(Json::parse(classJson(*entry, false)));
        else array.add(Json::object().set("name", child));
    }
    return array.dump();
}

std::string OSVMDebugSession::getSuperclasses(const std::string& name) const {
    if (!image) return "[]";
    uint64_t classOop = image->resolveClassOop(name);
    if (classOop == 0) return "[]";

    auto array = Json::array();
    for (SpurClassDescription current(image->behaviorAt(classOop).superclass().heapObject());
         current;
         current = SpurClassDescription(current.superclass().heapObject())) {
        const std::string className = current.name();
        const auto* entry = image->findClass(className);
        if (entry != nullptr) array.add(Json::parse(classJson(*entry, false)));
        else array.add(Json::object().set("name", className));
    }
    return array.dump();
}

std::string OSVMDebugSession::getVariables(const std::string& name) const {
    if (!image) return "[]";
    const uint64_t classOop = image->resolveClassOop(name);
    if (classOop == 0) return "[]";

    auto array = Json::array();
    for (const auto& variable : image->allInstanceVariableNamesOf(classOop)) {
        array.add(Json::object()
            .set("name", variable)
            .set("class", name)
            .set("type", "instance"));
    }
    for (const auto& [variable, value] : image->classVariablesOf(classOop)) {
        array.add(Json::object()
            .set("name", variable)
            .set("class", name)
            .set("type", "class")
            .set("value", image->objectAt(value).printString()));
    }
    return array.dump();
}

std::string OSVMDebugSession::getInstanceVariables(const std::string& name) const {
    if (!image) return "[]";
    const uint64_t classOop = image->resolveClassOop(name);
    if (classOop == 0) return "[]";

    auto array = Json::array();
    for (const auto& variable : image->instanceVariableNamesOf(classOop))
        array.add(Json::object().set("name", variable).set("class", name).set("type", "instance"));
    return array.dump();
}

std::string OSVMDebugSession::getClassVariables(const std::string& name) const {
    if (!image) return "[]";
    const uint64_t classOop = image->resolveClassOop(name);
    if (classOop == 0) return "[]";

    auto array = Json::array();
    for (const auto& [variable, value] : image->classVariablesOf(classOop)) {
        array.add(Json::object()
            .set("name", variable)
            .set("class", name)
            .set("type", "class")
            .set("value", image->objectAt(value).printString()));
    }
    return array.dump();
}

std::string OSVMDebugSession::getCategories(const std::string& name) const {
    if (!image) return "[]";
    const uint64_t classOop = image->resolveClassOop(name);
    if (classOop == 0) return "[]";

    auto array = Json::array();
    for (const auto& category : image->categoriesOf(classOop)) array.add(category);
    return array.dump();
}

std::string OSVMDebugSession::getUsedCategories(const std::string& name) const {
    if (!image) return "[]";
    const uint64_t classOop = image->resolveClassOop(name);
    if (classOop == 0) return "[]";

    std::vector<std::string> used;
    for (const auto& method : image->methodsOf(classOop)) {
        if (method.category.empty()) continue;
        if (std::find(used.begin(), used.end(), method.category) == used.end())
            used.push_back(method.category);
    }
    std::sort(used.begin(), used.end());

    auto array = Json::array();
    for (const auto& category : used) array.add(category);
    return array.dump();
}

std::string OSVMDebugSession::getSelectors(const std::string& name) const {
    if (!image) return "[]";
    const uint64_t classOop = image->resolveClassOop(name);
    if (classOop == 0) return "[]";

    auto array = Json::array();
    for (const auto& method : image->methodsOf(classOop)) array.add(method.selector);
    return array.dump();
}

std::string OSVMDebugSession::methodJson(uint64_t classOop, const std::string& className,
                                         const std::string& selector, uint64_t methodOop) const {
    const auto header = image->headerOfMethod(methodOop);
    return Json::object()
        .set("selector", selector)
        .set("methodClass", className)
        .set("class", className)
        .set("category", image->categoryOfMethod(classOop, selector))
        .set("source", image->sourceOfMethod(methodOop))
        .set("author", "")
        .set("timestamp", "")
        .set("package", "")
        .set("oop", Json::hex(methodOop))
        .set("numArgs", static_cast<int64_t>(header.numArgs))
        .set("numTemps", static_cast<int64_t>(header.numTemps))
        .dump();
}

std::string OSVMDebugSession::getMethods(const std::string& name) const {
    if (!image) return "[]";
    const uint64_t classOop = image->resolveClassOop(name);
    if (classOop == 0) return "[]";

    auto array = Json::array();
    for (const auto& method : image->methodsOf(classOop))
        array.add(Json::parse(methodJson(classOop, name, method.selector, method.oop)));
    return array.dump();
}

std::string OSVMDebugSession::getMethod(const std::string& className,
                                        const std::string& selector) const {
    if (!image) return "{}";
    const uint64_t classOop = image->resolveClassOop(className);
    if (classOop == 0) return "{}";
    const uint64_t methodOop = image->methodNamed(classOop, selector);
    if (methodOop == 0) return "{}";
    return methodJson(classOop, className, selector, methodOop);
}

std::string OSVMDebugSession::search(const std::string& text, bool ignoreCase,
                                     const std::string& condition,
                                     const std::string& type) const {
    if (!image) return "[]";
    auto array = Json::array();
    const bool wantClasses = type == "class" || type == "all" || type.empty();
    const bool wantSelectors = type == "selector" || type == "all" || type.empty();

    if (wantClasses) {
        for (const auto& [name, entry] : image->classes()) {
            if (!matches(name, text, ignoreCase, condition)) continue;
            array.add(Json::object().set("text", name).set("type", "class"));
        }
    }

    if (wantSelectors) {
        std::vector<std::string> seen;
        for (const auto& [name, entry] : image->classes()) {
            for (uint64_t side : {entry.oop, entry.metaclass}) {
                for (const auto& method : image->methodsOf(side)) {
                    if (!matches(method.selector, text, ignoreCase, condition)) continue;
                    if (std::find(seen.begin(), seen.end(), method.selector) != seen.end()) continue;
                    seen.push_back(method.selector);
                    array.add(Json::object().set("text", method.selector).set("type", "selector"));
                }
            }
        }
    }
    return array.dump();
}

// An image opened off disk has no process behind it, so it is neither
// stopped nor running.
std::string OSVMDebugSession::getStopReason() const {
    if (!isActive() && imageFile) return "image";
    return smalltalk::Session::getStopReason();
}

} // namespace osvm
