#include "SpurKnownObjects.h"
#include "SpurImage.h"
#include "CogFormat.h"
#include <algorithm>

namespace osvm {

namespace {

std::vector<std::string> stringsOf(const SpurHeapObject& array) {
    std::vector<std::string> names;
    if (!array.isPresent()) return names;
    const uint64_t count = array.numSlots();
    for (uint64_t i = 0; i < count; i++) {
        const std::string name = array.heapSlotAt(i).stringValue();
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

} // namespace

// ---------------------------------------------------------------------------
// SpurBehavior
// ---------------------------------------------------------------------------

SpurBehavior SpurBehavior::superclass() const {
    return SpurBehavior(heap.heapSlotAt(SpurBehavior::Slot::SUPERCLASS));
}

SpurMethodDictionary SpurBehavior::methodDictionary() const {
    return SpurMethodDictionary(heap.heapSlotAt(SpurBehavior::Slot::METHOD_DICT));
}

int64_t SpurBehavior::formatValue() const {
    return heap.slotAt(SpurBehavior::Slot::FORMAT).asSmallInteger().valueOr(0);
}

uint32_t SpurBehavior::instSpec() const {
    return BehaviorFormat::instSpec(formatValue());
}

uint32_t SpurBehavior::fixedFields() const {
    return BehaviorFormat::fixedFields(formatValue());
}

// ---------------------------------------------------------------------------
// SpurClassDescription
// ---------------------------------------------------------------------------

std::vector<std::string> SpurClassDescription::instanceVariableNames() const {
    if (!heap.isPresent()) return {};
    return stringsOf(heap.heapSlotAt(SpurClassDescription::Slot::INSTANCE_VARIABLES));
}

std::vector<std::string> SpurClassDescription::allInstanceVariableNames() const {
    std::vector<SpurClassDescription> chain;
    for (SpurClassDescription cls = *this; cls; cls = SpurClassDescription(cls.superclass().heapObject()))
        chain.push_back(cls);

    std::vector<std::string> names;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto own = it->instanceVariableNames();
        names.insert(names.end(), own.begin(), own.end());
    }
    return names;
}

SpurClassOrganizer SpurClassDescription::organization() const {
    return SpurClassOrganizer(heap.heapSlotAt(SpurClassDescription::Slot::ORGANIZATION));
}

std::string SpurClassDescription::name() const {
    if (!heap.isPresent()) return {};
    if (isMetaclass()) {
        const std::string base = SpurMetaclass(heap).thisClass().rawName();
        return base.empty() ? std::string() : base + " class";
    }
    return SpurClass(heap).rawName();
}

// ---------------------------------------------------------------------------
// SpurClass
// ---------------------------------------------------------------------------

std::string SpurClass::rawName() const {
    if (!heap.isPresent() || heap.numSlots() <= SpurClass::Slot::NAME) return {};
    return heap.heapSlotAt(SpurClass::Slot::NAME).stringValue();
}

std::vector<SpurClass> SpurClass::subclasses() const {
    std::vector<SpurClass> found;
    if (!heap.isPresent() || heap.numSlots() <= SpurClass::Slot::SUBCLASSES) return found;

    const SpurHeapObject array = heap.heapSlotAt(SpurClass::Slot::SUBCLASSES);
    if (!array.isPresent()) return found;

    const uint64_t count = array.numSlots();
    for (uint64_t i = 0; i < count; i++) {
        const SpurClass subclass(array.heapSlotAt(i));
        if (subclass) found.push_back(subclass);
    }
    return found;
}

std::vector<std::string> SpurClass::subclassNames() const {
    std::vector<std::string> names;
    for (const auto& subclass : subclasses()) {
        const std::string name = subclass.name();
        if (!name.empty()) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::pair<std::string, SpurObject>> SpurClass::classVariables() const {
    std::vector<std::pair<std::string, SpurObject>> vars;
    if (!heap.isPresent() || heap.numSlots() <= SpurClass::Slot::CLASS_POOL) return vars;

    const SpurHeapObject pool = heap.heapSlotAt(SpurClass::Slot::CLASS_POOL);
    if (!pool.isPresent()) return vars;

    const SpurHeapObject table = pool.heapSlotAt(SpurSystemDictionary::Slot::ARRAY);
    if (!table.isPresent()) return vars;

    const uint64_t count = table.numSlots();
    for (uint64_t i = 0; i < count; i++) {
        const SpurAssociation association(table.heapSlotAt(i));
        if (!association.isComplete()) continue;
        const std::string key = association.keyName();
        if (!key.empty()) vars.emplace_back(key, association.value());
    }
    std::sort(vars.begin(), vars.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return vars;
}

// ---------------------------------------------------------------------------
// SpurMetaclass
// ---------------------------------------------------------------------------

SpurClass SpurMetaclass::thisClass() const {
    return SpurClass(heap.heapSlotAt(SpurMetaclass::Slot::THIS_CLASS));
}

// ---------------------------------------------------------------------------
// SpurMethodDictionary
// ---------------------------------------------------------------------------

int64_t SpurMethodDictionary::tally() const {
    return heap.slotAt(SpurMethodDictionary::Slot::TALLY).asSmallInteger().valueOr(0);
}

SpurHeapObject SpurMethodDictionary::values() const {
    return heap.heapSlotAt(SpurMethodDictionary::Slot::ARRAY);
}

std::vector<std::pair<std::string, SpurCompiledMethod>>
SpurMethodDictionary::entries() const {
    std::vector<std::pair<std::string, SpurCompiledMethod>> found;
    if (!heap.isPresent()) return found;

    const SpurHeapObject methods = values();
    if (!methods.isPresent()) return found;

    const uint64_t capacity = heap.numSlots();
    const uint64_t methodCount = methods.numSlots();
    for (uint64_t i = SpurMethodDictionary::Slot::FIXED; i < capacity; i++) {
        const SpurHeapObject selector = heap.heapSlotAt(i);
        if (!selector.isPresent()) continue;

        const uint64_t index = i - SpurMethodDictionary::Slot::FIXED;
        if (index >= methodCount) continue;

        const std::string name = selector.stringValue();
        if (name.empty()) continue;
        found.emplace_back(name, SpurCompiledMethod(methods.heapSlotAt(index)));
    }
    return found;
}

// ---------------------------------------------------------------------------
// SpurCompiledMethod
// ---------------------------------------------------------------------------

// A Cog VM that has compiled a method keeps its machine code's address in the
// header slot, and the header itself in that code's own header.
int64_t SpurCompiledMethod::headerValue() const {
    const SpurObject raw = heap.slotAt(SpurCompiledMethod::Slot::HEADER);
    if (raw.isSmallInteger()) return raw.asSmallInteger().value();
    const uint64_t cogged = heap.image()->memoryReader()->readWord(raw.oop() + Cog::COG_METHOD_HEADER);
    return Spur::isSmallInteger(cogged) ? SpurSmallInteger(cogged, heap.image()).value() : 0;
}

SpurCompiledMethod::Header SpurCompiledMethod::header() const {
    Header decoded;
    if (!heap.isPresent()) return decoded;

    const int64_t value = headerValue();
    decoded.numLiterals = static_cast<uint32_t>(value & NUM_LITERALS_MASK);
    decoded.hasPrimitive = ((value >> PRIMITIVE_SHIFT) & 1) != 0;
    decoded.numTemps = static_cast<uint32_t>((value >> NUM_TEMPS_SHIFT) & NUM_TEMPS_MASK);
    decoded.numArgs = static_cast<uint32_t>((value >> NUM_ARGS_SHIFT) & NUM_ARGS_MASK);
    return decoded;
}

std::vector<SpurObject> SpurCompiledMethod::literals() const {
    std::vector<SpurObject> found;
    if (!heap.isPresent()) return found;

    const uint32_t count = header().numLiterals;
    const uint64_t slots = heap.numSlots();
    for (uint32_t i = SpurCompiledMethod::Slot::FIRST_LITERAL;
         i <= count && i < slots; i++)
        found.push_back(heap.slotAt(i));
    return found;
}

uint64_t SpurCompiledMethod::sourcePointer() const {
    if (!heap.isPresent()) return 0;

    const std::string body = heap.bytesAsString();
    if (body.size() < TRAILER_BYTES) return 0;

    const auto byteAt = [&](size_t fromEnd) {
        return static_cast<uint64_t>(static_cast<uint8_t>(body[body.size() - fromEnd]));
    };
    const uint64_t flag = byteAt(1);
    if (flag <= TRAILER_BASE) return 0;

    uint64_t pointer = flag - TRAILER_BASE;
    for (size_t i = 2; i <= TRAILER_BYTES; i++)
        pointer = pointer * 256 + byteAt(i);
    return pointer;
}

std::string SpurCompiledMethod::sourceCode() const {
    const uint64_t pointer = sourcePointer();
    if (pointer == 0) return {};
    return image()->sourceFiles().chunkAt(pointer);
}

// ---------------------------------------------------------------------------
// SpurAssociation
// ---------------------------------------------------------------------------

SpurObject SpurAssociation::key() const {
    return heap.slotAt(SpurAssociation::Slot::KEY);
}

SpurObject SpurAssociation::value() const {
    return heap.slotAt(SpurAssociation::Slot::VALUE);
}

std::string SpurAssociation::keyName() const {
    return heap.heapSlotAt(SpurAssociation::Slot::KEY).stringValue();
}

// ---------------------------------------------------------------------------
// SpurSystemDictionary
// ---------------------------------------------------------------------------

int64_t SpurSystemDictionary::tally() const {
    return heap.slotAt(SpurSystemDictionary::Slot::TALLY).asSmallInteger().valueOr(0);
}

SpurHeapObject SpurSystemDictionary::table() const {
    return heap.heapSlotAt(SpurSystemDictionary::Slot::ARRAY);
}

std::vector<SpurAssociation> SpurSystemDictionary::associations() const {
    std::vector<SpurAssociation> found;
    const SpurHeapObject entries = table();
    if (!entries.isPresent()) return found;

    const uint64_t count = entries.numSlots();
    for (uint64_t i = 0; i < count; i++) {
        const SpurAssociation association(entries.heapSlotAt(i));
        if (association.isComplete()) found.push_back(association);
    }
    return found;
}

// ---------------------------------------------------------------------------
// SpurCategorizer
// ---------------------------------------------------------------------------

std::vector<std::string> SpurCategorizer::categories() const {
    if (!heap.isPresent()) return {};
    return stringsOf(heap.heapSlotAt(SpurCategorizer::Slot::CATEGORY_ARRAY));
}

std::vector<std::pair<std::string, std::string>>
SpurCategorizer::elementCategories() const {
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!heap.isPresent()) return pairs;

    const SpurHeapObject names = heap.heapSlotAt(SpurCategorizer::Slot::CATEGORY_ARRAY);
    const SpurHeapObject stops = heap.heapSlotAt(SpurCategorizer::Slot::CATEGORY_STOPS);
    const SpurHeapObject elements = heap.heapSlotAt(SpurCategorizer::Slot::ELEMENT_ARRAY);
    if (!names.isPresent() || !stops.isPresent() || !elements.isPresent()) return pairs;

    const uint64_t categoryCount = names.numSlots();
    const uint64_t elementCount = elements.numSlots();

    uint64_t category = 0;
    for (uint64_t i = 0; i < elementCount; i++) {
        // Stops are 1-based inclusive ends into the element array.
        while (category < categoryCount) {
            const SpurSmallInteger stop = stops.slotAt(category).asSmallInteger();
            if (stop.isSmallInteger() &&
                static_cast<uint64_t>(stop.value()) >= i + 1) break;
            category++;
        }
        if (category >= categoryCount) break;

        const std::string element = elements.heapSlotAt(i).stringValue();
        if (element.empty()) continue;
        pairs.emplace_back(element, names.heapSlotAt(category).stringValue());
    }
    return pairs;
}

std::string SpurCategorizer::categoryOfElement(const std::string& element) const {
    for (const auto& [name, category] : elementCategories())
        if (name == element) return category;
    return {};
}

// ---------------------------------------------------------------------------
// SpurClassOrganizer
// ---------------------------------------------------------------------------

std::string SpurClassOrganizer::classComment() const {
    if (!heap.isPresent() || heap.numSlots() <= SpurClassOrganizer::Slot::CLASS_COMMENT)
        return {};

    const SpurHeapObject comment = heap.heapSlotAt(SpurClassOrganizer::Slot::CLASS_COMMENT);
    if (!comment.isPresent()) return {};
    if (comment.isBytes()) return comment.bytesAsString();
    return SpurRemoteString(comment).text();
}

// ---------------------------------------------------------------------------
// SpurRemoteString
// ---------------------------------------------------------------------------

uint32_t SpurRemoteString::sourceFileNumber() const {
    return static_cast<uint32_t>(
        heap.slotAt(SpurRemoteString::Slot::SOURCE_FILE_NUMBER).asSmallInteger().valueOr(0));
}

uint64_t SpurRemoteString::filePosition() const {
    return static_cast<uint64_t>(
        heap.slotAt(SpurRemoteString::Slot::FILE_POSITION).asSmallInteger().valueOr(0));
}

std::string SpurRemoteString::text() const {
    if (!heap.isPresent() || heap.numSlots() < 2) return {};
    return image()->sourceFiles().chunkIn(sourceFileNumber(), filePosition());
}

// ---------------------------------------------------------------------------
// SpurContext
// ---------------------------------------------------------------------------

SpurContext SpurContext::sender() const {
    return SpurContext(heap.heapSlotAt(SpurContext::Slot::SENDER));
}

SpurObject SpurContext::programCounter() const {
    return heap.slotAt(SpurContext::Slot::PC);
}

SpurObject SpurContext::stackPointer() const {
    return heap.slotAt(SpurContext::Slot::STACKP);
}

SpurCompiledMethod SpurContext::method() const {
    return SpurCompiledMethod(heap.heapSlotAt(SpurContext::Slot::METHOD));
}

SpurObject SpurContext::closure() const {
    return heap.slotAt(SpurContext::Slot::CLOSURE);
}

SpurObject SpurContext::receiver() const {
    return heap.slotAt(SpurContext::Slot::RECEIVER);
}

bool SpurContext::isBlockContext() const {
    return heap.heapSlotAt(SpurContext::Slot::CLOSURE).isPresent();
}

std::vector<SpurObject> SpurContext::temporaries() const {
    std::vector<SpurObject> found;
    if (!heap.isPresent()) return found;

    const uint64_t indexed = heap.numSlots() - SpurContext::Slot::FIXED_SLOTS;
    const uint64_t stackp =
        static_cast<uint64_t>(stackPointer().asSmallInteger().valueOr(indexed));
    const uint64_t live = std::min<uint64_t>(stackp, indexed);

    for (uint64_t i = 0; i < live; i++)
        found.push_back(heap.slotAt(SpurContext::Slot::FIXED_SLOTS + i));
    return found;
}

// ---------------------------------------------------------------------------
// SpurProcess
// ---------------------------------------------------------------------------

SpurProcess SpurProcess::nextLink() const {
    return SpurProcess(heap.heapSlotAt(SpurProcess::Slot::NEXT_LINK));
}

SpurContext SpurProcess::suspendedContext() const {
    return SpurContext(heap.heapSlotAt(SpurProcess::Slot::SUSPENDED_CONTEXT));
}

int SpurProcess::priority() const {
    return static_cast<int>(heap.slotAt(SpurProcess::Slot::PRIORITY).asSmallInteger().valueOr(0));
}

std::string SpurProcess::name() const {
    return heap.heapSlotAt(SpurProcess::Slot::NAME).stringValue();
}

// ---------------------------------------------------------------------------
// SpurLinkedList
// ---------------------------------------------------------------------------

SpurProcess SpurLinkedList::firstLink() const {
    return SpurProcess(heap.heapSlotAt(SpurLinkedList::Slot::FIRST_LINK));
}

std::vector<SpurProcess> SpurLinkedList::links() const {
    std::vector<SpurProcess> found;
    for (SpurProcess link = firstLink(); link; link = link.nextLink())
        found.push_back(link);
    return found;
}

// ---------------------------------------------------------------------------
// SpurProcessorScheduler
// ---------------------------------------------------------------------------

SpurProcess SpurProcessorScheduler::activeProcess() const {
    return SpurProcess(heap.heapSlotAt(SpurProcessorScheduler::Slot::ACTIVE_PROCESS));
}

std::vector<SpurProcess> SpurProcessorScheduler::quiescentProcesses() const {
    std::vector<SpurProcess> found;
    const SpurHeapObject lists =
        heap.heapSlotAt(SpurProcessorScheduler::Slot::QUIESCENT_PROCESS_LISTS);
    if (!lists.isPresent()) return found;

    const uint64_t count = lists.numSlots();
    for (uint64_t i = 0; i < count; i++) {
        const SpurLinkedList list(lists.heapSlotAt(i));
        if (!list) continue;
        for (const auto& process : list.links()) found.push_back(process);
    }
    return found;
}

} // namespace osvm
