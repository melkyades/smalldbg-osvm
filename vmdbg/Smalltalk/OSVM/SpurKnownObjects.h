#pragma once

#include "SpurHeapObject.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace osvm {

class SpurCompiledMethod;
class SpurClassOrganizer;
class SpurMethodDictionary;

// ---------------------------------------------------------------------------
// SpurKnownObject -- base for all typed heap-object wrappers.
//
// Holds the underlying SpurHeapObject and provides identity / validity.
// Subclasses add domain-specific slot accessors. Each declares only the slots
// it introduces, so the wrapper hierarchy follows the image's own: Behavior,
// then ClassDescription, then Class and Metaclass.
// ---------------------------------------------------------------------------
class SpurKnownObject {
public:
    SpurKnownObject() = default;
    explicit SpurKnownObject(SpurHeapObject h) : heap(h) {}

    explicit operator bool() const { return heap.isPresent(); }
    SpurHeapObject heapObject() const { return heap; }
    uint64_t oop() const { return heap.oop(); }
    const SpurImage* image() const { return heap.image(); }

protected:
    SpurHeapObject heap;
};

// ---------------------------------------------------------------------------
// SpurBehavior -- superclass, methodDict, format.
// ---------------------------------------------------------------------------
class SpurBehavior : public SpurKnownObject {
public:
    SpurBehavior() = default;
    explicit SpurBehavior(SpurHeapObject obj) : SpurKnownObject(obj) {}

    SpurBehavior superclass() const;
    SpurMethodDictionary methodDictionary() const;

    /** Returns the format instance variable: instSpec in the high bits, the fixed field
     *  count in the low 16. **/
    int64_t formatValue() const;
    uint32_t instSpec() const;
    uint32_t fixedFields() const;

    struct Slot {
        static constexpr uint64_t SUPERCLASS  = 0;
        static constexpr uint64_t METHOD_DICT = 1;
        static constexpr uint64_t FORMAT      = 2;
    };
};

// ---------------------------------------------------------------------------
// SpurClassDescription -- adds instanceVariables and organization.
// ---------------------------------------------------------------------------
class SpurClassDescription : public SpurBehavior {
public:
    SpurClassDescription() = default;
    explicit SpurClassDescription(SpurHeapObject obj) : SpurBehavior(obj) {}

    std::vector<std::string> instanceVariableNames() const;
    /** Returns every superclass's instance variables followed by its own, in slot order. **/
    std::vector<std::string> allInstanceVariableNames() const;
    SpurClassOrganizer organization() const;

    bool isMetaclass() const { return heap.isMetaclass(); }
    /** Returns the full name, with the " class" suffix when this is a metaclass. **/
    std::string name() const;

    struct Slot {
        static constexpr uint64_t INSTANCE_VARIABLES = 3;
        static constexpr uint64_t ORGANIZATION       = 4;
    };
};

// ---------------------------------------------------------------------------
// SpurClass -- adds subclasses, name, classPool and sharedPools.
//
// Cuis stops there; Squeak carries further slots which nothing here reads.
// ---------------------------------------------------------------------------
class SpurClass : public SpurClassDescription {
public:
    SpurClass() = default;
    explicit SpurClass(SpurHeapObject obj) : SpurClassDescription(obj) {}

    std::string rawName() const;
    std::vector<SpurClass> subclasses() const;
    std::vector<std::string> subclassNames() const;
    /** Returns the class variables as (name, value) pairs, sorted by name. **/
    std::vector<std::pair<std::string, SpurObject>> classVariables() const;

    struct Slot {
        static constexpr uint64_t SUBCLASSES   = 5;
        static constexpr uint64_t NAME         = 6;
        static constexpr uint64_t CLASS_POOL   = 7;
        static constexpr uint64_t SHARED_POOLS = 8;
        static constexpr uint64_t MIN_SLOTS    = 9;
    };
};

// ---------------------------------------------------------------------------
// SpurMetaclass -- adds thisClass, in the slot Class uses for subclasses.
// ---------------------------------------------------------------------------
class SpurMetaclass : public SpurClassDescription {
public:
    SpurMetaclass() = default;
    explicit SpurMetaclass(SpurHeapObject obj) : SpurClassDescription(obj) {}

    SpurClass thisClass() const;

    struct Slot {
        static constexpr uint64_t THIS_CLASS = 5;
        static constexpr uint64_t MIN_SLOTS  = 6;
    };
};

// ---------------------------------------------------------------------------
// SpurMethodDictionary -- tally and a values array, then indexed selector
// slots running parallel to that array.
// ---------------------------------------------------------------------------
class SpurMethodDictionary : public SpurKnownObject {
public:
    SpurMethodDictionary() = default;
    explicit SpurMethodDictionary(SpurHeapObject obj) : SpurKnownObject(obj) {}

    int64_t tally() const;
    SpurHeapObject values() const;

    /** Returns the occupied entries as (selector, method) pairs, in slot order. **/
    std::vector<std::pair<std::string, SpurCompiledMethod>> entries() const;

    struct Slot {
        static constexpr uint64_t TALLY = 0;
        static constexpr uint64_t ARRAY = 1;
        static constexpr uint64_t FIXED = 2;
    };
};

// ---------------------------------------------------------------------------
// SpurCompiledMethod -- a header SmallInteger, then literals, then bytecodes
// and a trailer encoding where the source lives.
// ---------------------------------------------------------------------------
class SpurCompiledMethod : public SpurKnownObject {
public:
    SpurCompiledMethod() = default;
    explicit SpurCompiledMethod(SpurHeapObject obj) : SpurKnownObject(obj) {}

    struct Header {
        uint32_t numLiterals{0};
        uint32_t numArgs{0};
        uint32_t numTemps{0};
        bool hasPrimitive{false};
    };
    Header header() const;
    /** Returns the method's header word, read through its machine code when the JIT
     *  compiled it. **/
    int64_t headerValue() const;
    /** Returns the pc of the first bytecode, counted from 1 at the header word. **/
    int initialPC() const;
    /** Returns the bytecodes, without the trailer that follows them. **/
    std::vector<uint8_t> bytecodes() const;

    std::vector<SpurObject> literals() const;
    /** Returns the method's source pointer, or 0 when it has none. **/
    uint64_t sourcePointer() const;
    std::string sourceCode() const;

    struct Slot {
        static constexpr uint64_t HEADER        = 0;
        static constexpr uint64_t FIRST_LITERAL = 1;
    };

    // Header bit layout.
    static constexpr int64_t NUM_LITERALS_MASK  = 0x7FFF;
    static constexpr int     PRIMITIVE_SHIFT    = 16;
    static constexpr int     NUM_TEMPS_SHIFT    = 18;
    static constexpr int64_t NUM_TEMPS_MASK     = 0x3F;
    static constexpr int     NUM_ARGS_SHIFT     = 24;
    static constexpr int64_t NUM_ARGS_MASK      = 0x0F;

    // Trailer: the last byte is >= TRAILER_BASE + 1 when a source pointer is
    // present, and holds the most significant byte of it.
    static constexpr uint64_t TRAILER_BASE  = 251;
    static constexpr size_t   TRAILER_BYTES = 4;
};

// ---------------------------------------------------------------------------
// SpurAssociation -- key and value.
// ---------------------------------------------------------------------------
class SpurAssociation : public SpurKnownObject {
public:
    SpurAssociation() = default;
    explicit SpurAssociation(SpurHeapObject obj) : SpurKnownObject(obj) {}

    SpurObject key() const;
    SpurObject value() const;
    std::string keyName() const;
    bool isComplete() const { return heap.isPresent() && heap.numSlots() >= 2; }

    struct Slot {
        static constexpr uint64_t KEY   = 0;
        static constexpr uint64_t VALUE = 1;
    };
};

// ---------------------------------------------------------------------------
// SpurSystemDictionary -- Smalltalk itself: a Dictionary of Associations.
// ---------------------------------------------------------------------------
class SpurSystemDictionary : public SpurKnownObject {
public:
    SpurSystemDictionary() = default;
    explicit SpurSystemDictionary(SpurHeapObject obj) : SpurKnownObject(obj) {}

    int64_t tally() const;
    SpurHeapObject table() const;
    std::vector<SpurAssociation> associations() const;

    struct Slot {
        static constexpr uint64_t TALLY = 0;
        static constexpr uint64_t ARRAY = 1;
    };
};

// ---------------------------------------------------------------------------
// SpurCategorizer -- elements grouped into categories by cumulative 1-based
// stop indices. SystemOrganization categorizes class names; a class's own
// organization categorizes its selectors.
// ---------------------------------------------------------------------------
class SpurCategorizer : public SpurKnownObject {
public:
    SpurCategorizer() = default;
    explicit SpurCategorizer(SpurHeapObject obj) : SpurKnownObject(obj) {}

    std::vector<std::string> categories() const;
    std::string categoryOfElement(const std::string& element) const;
    /** Returns every element paired with its category, read in one pass. **/
    std::vector<std::pair<std::string, std::string>> elementCategories() const;

    struct Slot {
        static constexpr uint64_t CATEGORY_ARRAY = 0;
        static constexpr uint64_t CATEGORY_STOPS = 1;
        static constexpr uint64_t ELEMENT_ARRAY  = 2;
    };
};

// ---------------------------------------------------------------------------
// SpurClassOrganizer -- a Categorizer that also carries the class comment.
// ---------------------------------------------------------------------------
class SpurClassOrganizer : public SpurCategorizer {
public:
    SpurClassOrganizer() = default;
    explicit SpurClassOrganizer(SpurHeapObject obj) : SpurCategorizer(obj) {}

    /** Returns the comment text, reading it from the source file when it is stored as a
     *  RemoteString, as it usually is. **/
    std::string classComment() const;

    struct Slot {
        static constexpr uint64_t SUBJECT       = 3;
        static constexpr uint64_t CLASS_COMMENT = 4;
        static constexpr uint64_t COMMENT_STAMP = 5;
    };
};

// ---------------------------------------------------------------------------
// SpurRemoteString -- text held in a source file rather than in the image.
// Note the position is raw, not an encoded source pointer.
// ---------------------------------------------------------------------------
class SpurRemoteString : public SpurKnownObject {
public:
    SpurRemoteString() = default;
    explicit SpurRemoteString(SpurHeapObject obj) : SpurKnownObject(obj) {}

    uint32_t sourceFileNumber() const;
    uint64_t filePosition() const;
    std::string text() const;

    struct Slot {
        static constexpr uint64_t SOURCE_FILE_NUMBER = 0;
        static constexpr uint64_t FILE_POSITION      = 1;
    };
};

// ---------------------------------------------------------------------------
// SpurContext -- a MethodContext: fixed fields then indexed temporaries.
// ---------------------------------------------------------------------------
class SpurContext : public SpurKnownObject {
public:
    SpurContext() = default;
    explicit SpurContext(SpurHeapObject obj) : SpurKnownObject(obj) {}

    /** Returns the calling context, or none when there is none on the heap: a married
     *  context's callers are frames in the VM's stack zone. **/
    SpurContext sender() const;
    /** Returns true when the context stands for a frame in a Cog VM's stack zone. Its
     *  sender slot then holds that frame's address, tagged as a SmallInteger. **/
    bool isMarried() const;
    /** Returns the address of a married context's frame, or 0 for any other. **/
    uint64_t framePointer() const;
    SpurObject programCounter() const;
    SpurObject stackPointer() const;
    SpurCompiledMethod method() const;
    SpurObject closure() const;
    SpurObject receiver() const;

    bool isBlockContext() const;
    /** Returns the arguments and temporaries in use, up to stackp. **/
    std::vector<SpurObject> temporaries() const;

    struct Slot {
        static constexpr uint64_t SENDER      = 0;
        static constexpr uint64_t PC          = 1;
        static constexpr uint64_t STACKP      = 2;
        static constexpr uint64_t METHOD      = 3;
        static constexpr uint64_t CLOSURE     = 4;
        static constexpr uint64_t RECEIVER    = 5;
        static constexpr uint64_t FIXED_SLOTS = 6;
    };
};

// ---------------------------------------------------------------------------
// SpurProcess -- a Smalltalk Process, and a Link in a scheduler list.
// ---------------------------------------------------------------------------
class SpurProcess : public SpurKnownObject {
public:
    SpurProcess() = default;
    explicit SpurProcess(SpurHeapObject obj) : SpurKnownObject(obj) {}

    SpurProcess nextLink() const;
    SpurContext suspendedContext() const;
    int priority() const;
    std::string name() const;

    struct Slot {
        static constexpr uint64_t NEXT_LINK         = 0;
        static constexpr uint64_t SUSPENDED_CONTEXT = 1;
        static constexpr uint64_t PRIORITY          = 2;
        static constexpr uint64_t MY_LIST           = 3;
        static constexpr uint64_t NAME              = 6;
    };
};

// ---------------------------------------------------------------------------
// SpurLinkedList -- the scheduler's per-priority run queues.
// ---------------------------------------------------------------------------
class SpurLinkedList : public SpurKnownObject {
public:
    SpurLinkedList() = default;
    explicit SpurLinkedList(SpurHeapObject obj) : SpurKnownObject(obj) {}

    SpurProcess firstLink() const;
    std::vector<SpurProcess> links() const;

    struct Slot {
        static constexpr uint64_t FIRST_LINK = 0;
        static constexpr uint64_t LAST_LINK  = 1;
    };
};

// ---------------------------------------------------------------------------
// SpurProcessorScheduler -- Processor.
// ---------------------------------------------------------------------------
class SpurProcessorScheduler : public SpurKnownObject {
public:
    SpurProcessorScheduler() = default;
    explicit SpurProcessorScheduler(SpurHeapObject obj) : SpurKnownObject(obj) {}

    SpurProcess activeProcess() const;
    /** Returns the runnable processes only; processes blocked on a semaphore are on the
     *  semaphore's list instead. **/
    std::vector<SpurProcess> quiescentProcesses() const;

    struct Slot {
        static constexpr uint64_t QUIESCENT_PROCESS_LISTS = 0;
        static constexpr uint64_t ACTIVE_PROCESS          = 1;
    };
};

} // namespace osvm
