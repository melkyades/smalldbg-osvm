#include "SpurHeapObject.h"
#include "SpurImage.h"
#include "SpurKnownObjects.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace osvm {

namespace {

constexpr int MAX_PRINT_DEPTH = 2;
constexpr uint64_t MAX_INLINE_ELEMENTS = 8;
constexpr int MAX_DOUBLE_DIGITS = 17;

bool startsWithVowel(const std::string& name) {
    if (name.empty()) return false;
    const char first = name[0];
    return first == 'A' || first == 'E' || first == 'I' || first == 'O' || first == 'U';
}

bool looksIntegral(const std::string& text) {
    return text.find_first_of(".eEni") == std::string::npos;
}

/** Returns the shortest text that reads back as the same double, as a Smalltalk
 *  printString does, keeping a trailing ".0" on whole numbers. **/
std::string printDouble(double value) {
    char buffer[64];
    int digits = MAX_DOUBLE_DIGITS;
    for (int candidate = 1; candidate < MAX_DOUBLE_DIGITS; candidate++) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", candidate, value);
        if (std::strtod(buffer, nullptr) == value) {
            digits = candidate;
            break;
        }
    }

    // %g turns scientific once the decimal exponent reaches the precision, so
    // 30.0 would come out as 3e+01. Widen the precision to cover the integer
    // part for ordinary magnitudes and leave the extremes in exponent form.
    const double magnitude = std::fabs(value);
    if (magnitude >= 1.0 && magnitude < 1e16) {
        const int exponent = static_cast<int>(std::floor(std::log10(magnitude)));
        digits = std::max(digits, exponent + 1);
    }

    std::snprintf(buffer, sizeof(buffer), "%.*g", digits, value);
    std::string text(buffer);
    if (looksIntegral(text)) text += ".0";
    return text;
}

} // namespace

// ---------------------------------------------------------------------------
// SpurObject
// ---------------------------------------------------------------------------

// The image knows which debugger reads it; an object only carries a pointer to
// the image, so the constructor cannot be inline where SpurImage is still
// incomplete.
SpurHeapObject::SpurHeapObject(uint64_t address, const SpurImage* image)
    : smalltalk::HeapObject(address, image ? image->debugger() : nullptr),
      spurImage(image) {}

uint32_t SpurHeapObject::size() const {
    return static_cast<uint32_t>(isBytes() ? byteSize() : numSlots());
}

uint64_t SpurHeapObject::rawSlotAt(int index) const {
    return rawSlotAt(static_cast<uint64_t>(index));
}

std::unique_ptr<smalltalk::HeapObject> SpurHeapObject::classObject() const {
    SpurHeapObject behavior = spurClass();
    if (!behavior.isValid()) return nullptr;
    return std::make_unique<SpurHeapObject>(behavior);
}

// Slot 0 of a ClassDescription is its superclass.
std::unique_ptr<smalltalk::HeapObject> SpurHeapObject::superclassObject() const {
    SpurObject parent = slotAt(0);
    if (!parent.isHeapObject()) return nullptr;
    return std::make_unique<SpurHeapObject>(parent.oop(), spurImage);
}

std::unique_ptr<smalltalk::Object> SpurHeapObject::slotObjectAt(int index) const {
    return std::make_unique<SpurObject>(slotAt(static_cast<uint64_t>(index)));
}

std::string SpurHeapObject::classNameIfClass() const {
    return nameIfClass();
}

bool SpurHeapObject::isNil() const { return spurImage && spurImage->isNil(rawOop); }
bool SpurHeapObject::isTrue() const { return spurImage && rawOop == spurImage->trueObject(); }
bool SpurHeapObject::isFalse() const { return spurImage && rawOop == spurImage->falseObject(); }

SpurObject::SpurObject(uint64_t oop, const SpurImage* image)
    : smalltalk::Object(oop, image ? image->debugger() : nullptr),
      spurImage(image) {}

bool SpurObject::isNil() const {
    return isValid() && rawOop == spurImage->nilObject();
}

bool SpurObject::isFalse() const {
    return isValid() && rawOop == spurImage->falseObject();
}

bool SpurObject::isTrue() const {
    return isValid() && rawOop == spurImage->trueObject();
}

SpurHeapObject SpurHeapObject::spurClass() const {
    if (!isValid()) return SpurHeapObject();
    return SpurHeapObject(spurImage->classOf(rawOop), spurImage);
}

SpurHeapObject SpurObject::spurClass() const {
    if (!isValid()) return SpurHeapObject();
    return SpurHeapObject(spurImage->classOf(rawOop), spurImage);
}

std::string SpurObject::className() const {
    return spurClass().nameIfClass();
}

std::string SpurObject::printString(int depth) const {
    if (!isValid() || rawOop == 0) return "<null>";
    if (isSmallInteger()) return std::to_string(asSmallInteger().value());
    if (isCharacter()) {
        const SpurCharacter character = asCharacter();
        const uint32_t code = character.value();
        if (code >= 32 && code < 127) return std::string("$") + static_cast<char>(code);
        const std::string utf8 = character.asUtf8();
        if (!utf8.empty()) return "$" + utf8;
        return "Character value: " + std::to_string(code);
    }
    if (isSmallFloat()) return printDouble(asSmallFloat().value());
    if (isNil()) return "nil";
    if (isFalse()) return "false";
    if (isTrue()) return "true";

    const SpurHeapObject self = asHeapObject();
    const std::string cls = className();

    if (self.isBytes()) {
        const std::string text = self.bytesAsString();
        if (cls == "Symbol" || cls == "ByteSymbol") return "#" + text;
        if (cls == "String" || cls == "ByteString") return "'" + text + "'";
        return "a " + (cls.empty() ? std::string("ByteArray") : cls);
    }
    if (self.isCompiledMethod()) return "a CompiledMethod";
    if (self.isClass() || self.isMetaclass()) return self.nameIfClass();

    if (depth < MAX_PRINT_DEPTH && self.isPointers()) {
        const uint64_t slots = self.numSlots();
        const bool isAssociation = cls == "Association";
        if ((cls == "Array" || isAssociation) && slots <= MAX_INLINE_ELEMENTS) {
            std::string out = isAssociation ? "" : "an Array(";
            for (uint64_t i = 0; i < slots; i++) {
                if (i > 0) out += isAssociation ? "->" : " ";
                out += self.slotAt(i).printString(depth + 1);
            }
            if (!isAssociation) out += ")";
            return out;
        }
    }

    if (cls.empty()) return "an Object";
    return (startsWithVowel(cls) ? "an " : "a ") + cls;
}

// ---------------------------------------------------------------------------
// SpurCharacter
// ---------------------------------------------------------------------------

std::string SpurCharacter::asUtf8() const {
    const uint32_t code = value();
    if (!isValidCodePoint(code)) return {};

    std::string out;
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    return out;
}

// ---------------------------------------------------------------------------
// SpurHeapObject
// ---------------------------------------------------------------------------

uint64_t SpurHeapObject::header() const {
    return spurImage->headerOf(rawOop);
}

uint32_t SpurHeapObject::classIndex() const {
    return static_cast<uint32_t>(header() & Spur::CLASS_INDEX_MASK);
}

uint32_t SpurHeapObject::format() const {
    return static_cast<uint32_t>((header() >> Spur::FORMAT_SHIFT) & Spur::FORMAT_MASK);
}

uint32_t SpurHeapObject::identityHash() const {
    return static_cast<uint32_t>((header() >> Spur::HASH_SHIFT) & Spur::HASH_MASK);
}

uint64_t SpurHeapObject::numSlots() const {
    return spurImage->numSlotsOf(rawOop);
}

uint64_t SpurHeapObject::rawSlotAt(uint64_t index) const {
    return spurImage->slotOf(rawOop, index);
}

SpurObject SpurHeapObject::slotAt(uint64_t index) const {
    return SpurObject(rawSlotAt(index), spurImage);
}

SpurHeapObject SpurHeapObject::heapSlotAt(uint64_t index) const {
    return SpurHeapObject(rawSlotAt(index), spurImage);
}

uint64_t SpurHeapObject::byteSize() const {
    return numSlots() * Spur::BYTES_PER_OOP - Fmt::oddBytes(format());
}

std::string SpurHeapObject::bytesAsString() const {
    return spurImage->bytesOf(rawOop);
}

std::string SpurHeapObject::stringValue() const {
    if (!isPresent() || !isBytes()) return {};
    return bytesAsString();
}

bool SpurHeapObject::isMetaclass() const {
    return isPresent() && spurImage->classOf(rawOop) == spurImage->metaclassObject();
}

bool SpurHeapObject::isClass() const {
    return isPresent() && spurClass().isMetaclass();
}

std::string SpurHeapObject::nameIfClass() const {
    return SpurClassDescription(*this).name();
}

std::string SpurHeapObject::rawNameSlot() const {
    return SpurClass(*this).rawName();
}

SpurHeapObject SpurHeapObject::nextObject() const {
    return SpurHeapObject(spurImage->objectAfter(rawOop), spurImage);
}

} // namespace osvm
