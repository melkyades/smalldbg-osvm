#include "SpurImage.h"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace osvm {

namespace {

constexpr uint32_t CLASS_TABLE_PAGES = 4096;
constexpr uint64_t MAX_REASONABLE_BYTES = 1u << 28;

std::string joinWithSpaces(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += ' ';
        out += part;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// raw object access -- the primitive layer SpurObject / SpurHeapObject use
// ---------------------------------------------------------------------------

uint32_t SpurImage::formatOf(uint64_t oop) const {
    return static_cast<uint32_t>((headerOf(oop) >> Spur::FORMAT_SHIFT) & Spur::FORMAT_MASK);
}

uint32_t SpurImage::classIndexOf(uint64_t oop) const {
    return static_cast<uint32_t>(headerOf(oop) & Spur::CLASS_INDEX_MASK);
}

uint64_t SpurImage::numSlotsOf(uint64_t oop) const {
    const uint64_t slots = (headerOf(oop) >> Spur::SLOTS_SHIFT) & Spur::SLOTS_OVERFLOW;
    if (slots != Spur::SLOTS_OVERFLOW) return slots;
    return memory->readWord(oop - Spur::BASE_HEADER_SIZE) & Spur::OVERFLOW_MASK;
}

uint64_t SpurImage::slotOf(uint64_t oop, uint64_t index) const {
    return memory->readWord(oop + Spur::BASE_HEADER_SIZE + index * Spur::BYTES_PER_OOP);
}

uint64_t SpurImage::byteSizeOf(uint64_t oop) const {
    return numSlotsOf(oop) * Spur::BYTES_PER_OOP - Fmt::oddBytes(formatOf(oop));
}

std::string SpurImage::bytesOf(uint64_t oop) const {
    const uint64_t size = byteSizeOf(oop);
    if (size == 0 || size > MAX_REASONABLE_BYTES) return {};
    std::string out(static_cast<size_t>(size), '\0');
    if (!memory->readBytes(oop + Spur::BASE_HEADER_SIZE, out.data(), out.size())) return {};
    return out;
}

uint64_t SpurImage::objectAfter(uint64_t oop) const {
    uint64_t next = oop + Spur::objectBytesForSlots(numSlotsOf(oop));
    const uint64_t header = memory->readWord(next);
    if (((header >> Spur::SLOTS_SHIFT) & Spur::SLOTS_OVERFLOW) == Spur::SLOTS_OVERFLOW)
        next += Spur::BASE_HEADER_SIZE;
    return next;
}

// ---------------------------------------------------------------------------
// identity
// ---------------------------------------------------------------------------

uint64_t SpurImage::specialObject(uint64_t index) const {
    return slotOf(imageRoots.specialObjectsOop, index);
}

uint64_t SpurImage::classAtIndex(uint32_t index) const {
    const SpurHeapObject page =
        heapObjectAt(imageRoots.hiddenRootsObj).heapSlotAt(index >> Spur::CLASS_TABLE_PAGE_SHIFT);
    if (!page.isPresent()) return 0;
    const SpurHeapObject entry = page.heapSlotAt(index & Spur::CLASS_TABLE_PAGE_MASK);
    return entry.isPresent() ? entry.oop() : 0;
}

uint64_t SpurImage::classOf(uint64_t oop) const {
    if (Spur::isImmediate(oop)) return classAtIndex(Spur::immediateClassIndex(oop));
    return classAtIndex(classIndexOf(oop));
}

bool SpurImage::isMetaclass(uint64_t oop) const {
    return heapObjectAt(oop).isMetaclass();
}

bool SpurImage::isClass(uint64_t oop) const {
    return heapObjectAt(oop).isClass();
}

std::string SpurImage::stringOf(uint64_t oop) const {
    return heapObjectAt(oop).stringValue();
}

std::string SpurImage::printStringOf(uint64_t oop, int depth) const {
    return objectAt(oop).printString(depth);
}

// ---------------------------------------------------------------------------
// loading
// ---------------------------------------------------------------------------

bool SpurImage::load() {
    imageRoots = memory->roots();
    if (!imageRoots.valid) {
        lastError = "object memory has no valid Spur roots";
        return false;
    }
    if (!locateMetaclass()) {
        lastError = "could not find Metaclass in the class table";
        return false;
    }

    smalltalkOop = specialObject(SpecialObject::SMALLTALK);
    if (!heapObjectAt(smalltalkOop).isPresent()) {
        lastError = "special objects array has no Smalltalk dictionary";
        return false;
    }

    indexSystemOrganization();
    discoverClasses();
    if (classCache.empty()) {
        lastError = "no classes found among Smalltalk globals";
        return false;
    }
    return true;
}

bool SpurImage::locateMetaclass() {
    // Metaclass is always in the class table, since its instances are the
    // metaclasses. Finding it once turns every later metaclass test into a
    // single pointer compare.
    const SpurHeapObject roots = heapObjectAt(imageRoots.hiddenRootsObj);
    for (uint32_t page = 0; page < CLASS_TABLE_PAGES; page++) {
        const SpurHeapObject pageObject = roots.heapSlotAt(page);
        if (!pageObject.isPresent()) continue;
        const uint64_t count = pageObject.numSlots();
        for (uint64_t i = 0; i < count; i++) {
            const SpurHeapObject candidate = pageObject.heapSlotAt(i);
            if (!candidate.isPresent()) continue;
            if (candidate.numSlots() < SpurClass::Slot::MIN_SLOTS) continue;
            if (SpurClass(candidate).rawName() == "Metaclass") {
                metaclassOop = candidate.oop();
                return true;
            }
        }
    }
    return false;
}

SpurProcessorScheduler SpurImage::scheduler() const {
    const SpurAssociation association(
        heapObjectAt(specialObject(SpecialObject::SCHEDULER_ASSOC)));
    if (!association.isComplete()) return SpurProcessorScheduler();
    return SpurProcessorScheduler(association.value().asHeapObject());
}

std::string SpurImage::rawClassName(uint64_t classOop) const {
    return heapObjectAt(classOop).rawNameSlot();
}

std::string SpurImage::nameOfClass(uint64_t classOop) const {
    return heapObjectAt(classOop).nameIfClass();
}

void SpurImage::globalsDo(const std::function<void(const std::string&, uint64_t)>& visit) const {
    for (const auto& association : systemDictionary().associations()) {
        const std::string key = association.keyName();
        if (key.empty()) continue;
        visit(key, association.value().oop());
    }
}

void SpurImage::discoverClasses() {
    globalsDo([this](const std::string& name, uint64_t value) {
        const SpurHeapObject candidate = heapObjectAt(value);
        if (!candidate.isClass()) return;
        ClassEntry entry;
        entry.name = name;
        entry.oop = value;
        entry.metaclass = classOf(value);
        entry.superclassName = superclassNameOf(value);
        entry.category = categoryOfClass(name);
        classCache[name] = entry;
    });
}

void SpurImage::indexSystemOrganization() {
    uint64_t organization = 0;
    globalsDo([&](const std::string& name, uint64_t value) {
        if (name == "SystemOrganization") organization = value;
    });

    const SpurCategorizer organizer(heapObjectAt(organization));
    if (!organizer) return;

    for (const auto& [className, category] : organizer.elementCategories())
        classCategories[className] = category;
}

// ---------------------------------------------------------------------------
// classes
// ---------------------------------------------------------------------------

const SpurImage::ClassEntry* SpurImage::findClass(const std::string& name) const {
    auto found = classCache.find(name);
    return found == classCache.end() ? nullptr : &found->second;
}

uint64_t SpurImage::resolveClassOop(const std::string& name) const {
    static const std::string suffix = " class";
    const bool meta = name.size() > suffix.size() &&
                      name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    const std::string base = meta ? name.substr(0, name.size() - suffix.size()) : name;

    const ClassEntry* entry = findClass(base);
    if (entry == nullptr) return 0;
    return meta ? entry->metaclass : entry->oop;
}

std::string SpurImage::superclassNameOf(uint64_t classOop) const {
    return behaviorAt(classOop).superclass().heapObject().nameIfClass();
}

std::vector<std::string> SpurImage::subclassNamesOf(uint64_t classOop) const {
    return classAt(classOop).subclassNames();
}

std::vector<std::string> SpurImage::instanceVariableNamesOf(uint64_t classOop) const {
    return behaviorAt(classOop).instanceVariableNames();
}

std::vector<std::string> SpurImage::allInstanceVariableNamesOf(uint64_t classOop) const {
    return behaviorAt(classOop).allInstanceVariableNames();
}

std::vector<std::pair<std::string, uint64_t>>
SpurImage::classVariablesOf(uint64_t classOop) const {
    std::vector<std::pair<std::string, uint64_t>> vars;
    for (const auto& [name, value] : classAt(classOop).classVariables())
        vars.emplace_back(name, value.oop());
    return vars;
}

std::string SpurImage::categoryOfClass(const std::string& className) const {
    auto found = classCategories.find(className);
    return found == classCategories.end() ? std::string() : found->second;
}

std::string SpurImage::remoteStringText(uint64_t remoteString) const {
    return SpurRemoteString(heapObjectAt(remoteString)).text();
}

std::string SpurImage::commentOfClass(uint64_t classOop) const {
    return behaviorAt(classOop).organization().classComment();
}

/// The subclass: keyword a class's instances call for.
static std::string subclassKeywordFor(uint32_t instSpec) {
    if (instSpec == Fmt::VARIABLE || instSpec == Fmt::FIXED_VARIABLE) return "variableSubclass:";
    if (instSpec == Fmt::WEAK_VARIABLE) return "weakSubclass:";
    if (instSpec >= Fmt::INDEXABLE_8 && instSpec < Fmt::COMPILED_METHOD) return "variableByteSubclass:";
    if (instSpec == Fmt::INDEXABLE_32 || instSpec == Fmt::INDEXABLE_64) return "variableWordSubclass:";
    return "subclass:";
}

std::string SpurImage::definitionOfClass(uint64_t classOop) const {
    const SpurClassDescription cls = behaviorAt(classOop);
    if (!cls) return {};

    const std::string name = cls.name();
    if (cls.isMetaclass()) {
        std::ostringstream out;
        out << name << "\n\tinstanceVariableNames: '"
            << joinWithSpaces(cls.instanceVariableNames()) << "'";
        return out.str();
    }

    std::string superclass = superclassNameOf(classOop);
    if (superclass.empty()) superclass = "nil";

    const std::string variableKeyword = subclassKeywordFor(cls.instSpec());

    std::vector<std::string> classVarNames;
    for (const auto& entry : SpurClass(cls.heapObject()).classVariables())
        classVarNames.push_back(entry.first);

    std::ostringstream out;
    out << superclass << ' ' << variableKeyword << " #" << name
        << "\n\tinstanceVariableNames: '" << joinWithSpaces(cls.instanceVariableNames()) << "'"
        << "\n\tclassVariableNames: '" << joinWithSpaces(classVarNames) << "'"
        << "\n\tpoolDictionaries: ''"
        << "\n\tcategory: '" << categoryOfClass(name) << "'";
    return out.str();
}

// ---------------------------------------------------------------------------
// methods
// ---------------------------------------------------------------------------

std::vector<SpurImage::MethodEntry> SpurImage::methodsOf(uint64_t classOop) const {
    std::vector<MethodEntry> methods;
    const SpurClassDescription cls = behaviorAt(classOop);
    if (!cls) return methods;

    // One pass over the organizer beats a scan per selector.
    std::map<std::string, std::string> categories;
    for (const auto& [selector, category] : cls.organization().elementCategories())
        categories[selector] = category;

    for (const auto& [selector, method] : cls.methodDictionary().entries()) {
        MethodEntry entry;
        entry.selector = selector;
        entry.oop = method.oop();
        auto category = categories.find(selector);
        if (category != categories.end()) entry.category = category->second;
        methods.push_back(entry);
    }

    std::sort(methods.begin(), methods.end(),
              [](const MethodEntry& a, const MethodEntry& b) { return a.selector < b.selector; });
    return methods;
}

uint64_t SpurImage::methodNamed(uint64_t classOop, const std::string& selector) const {
    for (const auto& entry : methodsOf(classOop))
        if (entry.selector == selector) return entry.oop;
    return 0;
}

SpurImage::MethodHeader SpurImage::headerOfMethod(uint64_t methodOop) const {
    const auto decoded = methodAt(methodOop).header();
    return MethodHeader{decoded.numLiterals, decoded.numArgs,
                        decoded.numTemps, decoded.hasPrimitive};
}

std::vector<uint64_t> SpurImage::literalsOf(uint64_t methodOop) const {
    std::vector<uint64_t> literals;
    for (const auto& literal : methodAt(methodOop).literals())
        literals.push_back(literal.oop());
    return literals;
}

std::string SpurImage::sourceOfMethod(uint64_t methodOop) const {
    return methodAt(methodOop).sourceCode();
}

std::string SpurImage::categoryFromOrganizer(uint64_t organizerOop,
                                             const std::string& element) const {
    return SpurCategorizer(heapObjectAt(organizerOop)).categoryOfElement(element);
}

void SpurImage::indexMethodLocations() const {
    for (const auto& [name, entry] : classCache) {
        for (const auto& method : methodsOf(entry.oop))
            methodLocations[method.oop] = MethodLocation{name, method.selector};
        for (const auto& method : methodsOf(entry.metaclass))
            methodLocations[method.oop] = MethodLocation{name + " class", method.selector};
    }
    methodLocationsIndexed = true;
}

const SpurImage::MethodLocation* SpurImage::locationOfMethod(uint64_t methodOop) const {
    if (!methodLocationsIndexed) indexMethodLocations();
    auto found = methodLocations.find(methodOop);
    return found == methodLocations.end() ? nullptr : &found->second;
}

std::string SpurImage::categoryOfMethod(uint64_t classOop, const std::string& selector) const {
    return behaviorAt(classOop).organization().categoryOfElement(selector);
}

std::vector<std::string> SpurImage::categoriesOf(uint64_t classOop) const {
    return behaviorAt(classOop).organization().categories();
}

} // namespace osvm
