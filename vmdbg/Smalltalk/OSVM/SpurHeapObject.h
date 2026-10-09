#pragma once

#include "SpurObject.h"
#include "Smalltalk/HeapObject.h"
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace osvm {

// ---------------------------------------------------------------------------
// SpurHeapObject -- handle to a heap-resident Spur object.
//
// Knows its address and holds the image it belongs to, so header fields,
// slots and bytes are read lazily through that image's memory backend --
// an image file or a live VM, indistinguishably.
// ---------------------------------------------------------------------------
class SpurHeapObject : public smalltalk::HeapObject {
public:
    SpurHeapObject() = default;
    SpurHeapObject(uint64_t address, const SpurImage* image);

    const SpurImage* image() const { return spurImage; }

    /// The same object seen as an untyped oop.
    operator SpurObject() const { return SpurObject(rawOop, spurImage); }

    // ---- encoding, as Spur tags it ------------------------------------
    size_t slotSize() const override { return Spur::BYTES_PER_OOP; }
    bool isImmediate() const override { return Spur::isImmediate(rawOop); }
    bool isSmallInteger() const override { return Spur::isSmallInteger(rawOop); }
    int64_t smallIntegerValue() const override { return Spur::smallIntegerValue(rawOop); }
    bool isCharacter() const override { return Spur::isCharacter(rawOop); }
    uint32_t characterValue() const override { return Spur::characterValue(rawOop); }
    bool isNil() const override;
    bool isTrue() const override;
    bool isFalse() const override;
    bool isPresent() const { return rawOop != 0 && !isNil(); }

    /// The receiver's class, as Spur records it in the class table.
    SpurHeapObject spurClass() const;

    // ---- what the shared rendering and kind tests need ----------------
    uint32_t size() const override;
    bool isBytes() const override { return Fmt::isBytes(format()); }
    uint64_t bodyAddress() const override { return rawOop + Spur::BASE_HEADER_SIZE; }
    uint64_t rawSlotAt(int index) const override;
    std::unique_ptr<smalltalk::HeapObject> classObject() const override;
    std::unique_ptr<smalltalk::HeapObject> superclassObject() const override;
    std::unique_ptr<smalltalk::Object> slotObjectAt(int index) const override;
    std::string classNameIfClass() const override;

    uint64_t address() const { return rawOop; }

    // ---- header accessors -------------------------------------------
    uint64_t header() const;
    uint32_t classIndex() const;
    uint32_t format() const;
    uint32_t identityHash() const;
    uint64_t numSlots() const;

    // ---- format helpers ---------------------------------------------
    bool isPointers() const { return Fmt::isPointers(format()); }
    bool isCompiledMethod() const { return Fmt::isMethod(format()); }

    // ---- slot accessors ---------------------------------------------
    uint64_t rawSlotAt(uint64_t index) const;
    SpurObject slotAt(uint64_t index) const;
    SpurHeapObject heapSlotAt(uint64_t index) const;

    // ---- byte access (String, Symbol, ByteArray, CompiledMethod) -----
    uint64_t byteSize() const;
    std::string bytesAsString() const;
    /// Text of a String or Symbol; empty for anything else.
    std::string stringValue() const;

    // ---- class resolution -------------------------------------------
    bool isClass() const;
    bool isMetaclass() const;

    /// Name of this object when it is itself a class or metaclass.
    std::string nameIfClass() const;
    /// Text of the `name` instance variable, without the metaclass suffix.
    std::string rawNameSlot() const;

    // ---- heap walking ------------------------------------------------
    SpurHeapObject nextObject() const;

protected:
    const SpurImage* spurImage{nullptr};
};

// ---------------------------------------------------------------------------
// Complete the inline down-cast declared in SpurObject.h
// ---------------------------------------------------------------------------
inline SpurHeapObject SpurObject::asHeapObject() const {
    return SpurHeapObject(rawOop, spurImage);
}

} // namespace osvm
