#pragma once

#include "SpurMemory.h"
#include "SpurFormat.h"
#include "SpurHeapObject.h"
#include "SpurKnownObjects.h"
#include "SpurSourceFiles.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace osvm {

/** A SpurImage reads Smalltalk objects from a Spur heap through a SpurMemory, so the same
 *  code serves an image file and a live VM. It finds classes through Smalltalk's globals,
 *  because Spur adds a class to the class table only once its identity hash is needed. **/
class SpurImage {
public:
    explicit SpurImage(SpurMemory* mem) : memory(mem) {}

    /** Reads the roots, finds Metaclass and indexes every global class. Returns false
     *  when a step fails, with the reason in error(). **/
    bool load();

    // ---- factories: wrap a raw oop as a typed object ----
    SpurHeapObject heapObjectAt(uint64_t address) const { return SpurHeapObject(address, this); }
    SpurObject objectAt(uint64_t oop) const { return SpurObject(oop, this); }
    SpurClass classAt(uint64_t oop) const { return SpurClass(heapObjectAt(oop)); }
    SpurClassDescription behaviorAt(uint64_t oop) const {
        return SpurClassDescription(heapObjectAt(oop));
    }
    SpurCompiledMethod methodAt(uint64_t oop) const {
        return SpurCompiledMethod(heapObjectAt(oop));
    }

    /** Returns the Smalltalk system dictionary. **/
    SpurSystemDictionary systemDictionary() const {
        return SpurSystemDictionary(heapObjectAt(smalltalkOop));
    }
    /** Returns Processor, found through the special objects array. **/
    SpurProcessorScheduler scheduler() const;

    /** Returns the debugger that reads the heap as plain bytes for the shared object
     *  model, or null when there is no memory. **/
    smalldbg::Debugger* debugger() const { return memory ? memory->debugger() : nullptr; }

    /** Returns the start of the readable heap, for the region map. **/
    uint64_t objectMemoryBase() const { return memory ? memory->baseAddress() : 0; }
    /** Returns the end of the readable heap, for the region map. **/
    uint64_t objectMemoryEnd() const { return memory ? memory->endAddress() : 0; }

    // ---- raw object access (the primitive layer Spur objects read through) ----
    uint64_t headerOf(uint64_t oop) const { return memory->readWord(oop); }
    uint32_t formatOf(uint64_t oop) const;
    uint32_t classIndexOf(uint64_t oop) const;
    uint64_t numSlotsOf(uint64_t oop) const;
    uint64_t slotOf(uint64_t oop, uint64_t index) const;
    uint64_t byteSizeOf(uint64_t oop) const;
    std::string bytesOf(uint64_t oop) const;
    uint64_t objectAfter(uint64_t oop) const;

    // ---- identity ----
    uint64_t nilObject() const { return imageRoots.nilObj; }
    uint64_t falseObject() const { return imageRoots.falseObj; }
    uint64_t trueObject() const { return imageRoots.trueObj; }
    uint64_t metaclassObject() const { return metaclassOop; }
    bool isNil(uint64_t oop) const { return oop == imageRoots.nilObj; }
    bool isPresent(uint64_t oop) const {
        return oop != 0 && !isNil(oop) && !Spur::isImmediate(oop);
    }
    uint64_t specialObject(uint64_t index) const;

    uint64_t classAtIndex(uint32_t index) const;
    uint64_t classOf(uint64_t oop) const;
    bool isMetaclass(uint64_t oop) const;
    bool isClass(uint64_t oop) const;

    // ---- strings ----
    /** Returns the text of a String or Symbol, or an empty string for anything else. **/
    std::string stringOf(uint64_t oop) const;
    /** Returns a one-line description of any oop, for inspectors and bindings. **/
    std::string printStringOf(uint64_t oop, int depth = 0) const;

    // ---- classes ----
    struct ClassEntry {
        std::string name;
        uint64_t oop{0};
        uint64_t metaclass{0};
        std::string superclassName;
        std::string category;
        bool isMeta{false};
    };

    const std::map<std::string, ClassEntry>& classes() const { return classCache; }
    const ClassEntry* findClass(const std::string& name) const;

    /** Returns the class named by a Webside class name, or its metaclass when the name
     *  ends in " class". Returns 0 when there is no such class. **/
    uint64_t resolveClassOop(const std::string& name) const;
    std::string nameOfClass(uint64_t classOop) const;

    std::string superclassNameOf(uint64_t classOop) const;
    std::vector<std::string> subclassNamesOf(uint64_t classOop) const;
    std::vector<std::string> instanceVariableNamesOf(uint64_t classOop) const;
    std::vector<std::string> allInstanceVariableNamesOf(uint64_t classOop) const;
    std::vector<std::pair<std::string, uint64_t>> classVariablesOf(uint64_t classOop) const;
    std::string categoryOfClass(const std::string& className) const;
    std::string commentOfClass(uint64_t classOop) const;
    std::string definitionOfClass(uint64_t classOop) const;

    // ---- methods ----
    struct MethodEntry {
        std::string selector;
        uint64_t oop{0};
        std::string category;
    };

    std::vector<MethodEntry> methodsOf(uint64_t classOop) const;
    uint64_t methodNamed(uint64_t classOop, const std::string& selector) const;
    std::string sourceOfMethod(uint64_t methodOop) const;
    std::string categoryOfMethod(uint64_t classOop, const std::string& selector) const;
    std::vector<std::string> categoriesOf(uint64_t classOop) const;

    /** Returns the literals of a method, including the selectors it sends, for senders
     *  and implementors. **/
    std::vector<uint64_t> literalsOf(uint64_t methodOop) const;

    /** A MethodLocation names the class and selector a CompiledMethod is installed under,
     *  for naming stack frames. **/
    struct MethodLocation {
        std::string className;
        std::string selector;
    };
    const MethodLocation* locationOfMethod(uint64_t methodOop) const;

    struct MethodHeader {
        uint32_t numLiterals{0};
        uint32_t numArgs{0};
        uint32_t numTemps{0};
        bool hasPrimitive{false};
    };
    MethodHeader headerOfMethod(uint64_t methodOop) const;

    SpurSourceFiles& sourceFiles() { return sources; }
    const SpurSourceFiles& sourceFiles() const { return sources; }
    SpurMemory* memoryReader() const { return memory; }
    const SpurRoots& rootObjects() const { return imageRoots; }
    const std::string& error() const { return lastError; }

    /** Calls `visit` with the name and value of every global, including non-class values. **/
    void globalsDo(const std::function<void(const std::string&, uint64_t)>& visit) const;

private:
    bool locateMetaclass();
    void discoverClasses();
    void indexSystemOrganization();
    std::string rawClassName(uint64_t classOop) const;
    std::string categoryFromOrganizer(uint64_t organizer, const std::string& element) const;
    uint64_t sourcePointerOf(uint64_t methodOop) const;
    std::string remoteStringText(uint64_t remoteString) const;

    SpurMemory* memory{nullptr};
    SpurRoots imageRoots;
    SpurSourceFiles sources;

    void indexMethodLocations() const;

    uint64_t metaclassOop{0};
    uint64_t smalltalkOop{0};
    std::map<std::string, ClassEntry> classCache;
    std::map<std::string, std::string> classCategories;
    mutable std::map<uint64_t, MethodLocation> methodLocations;
    mutable bool methodLocationsIndexed{false};
    std::string lastError;
};

} // namespace osvm
