#pragma once

#include "Smalltalk/Object.h"

#include "SpurFormat.h"
#include <cstdint>
#include <string>

namespace osvm {

class SpurImage;
class SpurHeapObject;
class SpurSmallInteger;
class SpurCharacter;
class SpurSmallFloat;

// ---------------------------------------------------------------------------
// SpurObject -- lightweight value type representing any Spur oop.
//
// An oop is either an immediate (low 3 tag bits non-zero: a 61-bit
// SmallInteger, a Character, or a SmallFloat64) or a pointer to a heap
// object. Carries the raw oop plus the image that gives it meaning, since
// resolving a class means going through that image's class table.
// ---------------------------------------------------------------------------
class SpurObject : public smalltalk::Object {
public:
    SpurObject() = default;
    SpurObject(uint64_t oop, const SpurImage* image);

    // ---- identity ---------------------------------------------------
    const SpurImage* image() const { return spurImage; }

    // ---- tag tests --------------------------------------------------
    size_t slotSize() const override { return 8; }
    bool isImmediate() const override { return Spur::isImmediate(rawOop); }
    bool isSmallInteger() const override { return Spur::isSmallInteger(rawOop); }
    bool isCharacter() const override { return Spur::isCharacter(rawOop); }
    bool isSmallFloat() const override { return Spur::isSmallFloat(rawOop); }

    bool isNil() const override;
    bool isFalse() const override;
    bool isTrue() const override;

    // ---- immediate values -------------------------------------------
    int64_t smallIntegerValue() const override { return Spur::smallIntegerValue(rawOop); }
    uint32_t characterValue() const override { return Spur::characterValue(rawOop); }
    double smallFloatValue() const override { return Spur::smallFloatValue(rawOop); }

    // ---- typed views ------------------------------------------------
    SpurSmallInteger asSmallInteger() const;   // defined below
    SpurCharacter asCharacter() const;         // defined below
    SpurSmallFloat asSmallFloat() const;       // defined below
    SpurHeapObject asHeapObject() const;       // defined in SpurHeapObject.h

    // ---- class ------------------------------------------------------
    SpurHeapObject spurClass() const;
    std::string className() const override;

    // ---- convenience -------------------------------------------------
    std::string printString(int depth) const;
    std::string printString() const override { return printString(0); }

protected:
    const SpurImage* spurImage{nullptr};
};

// ---------------------------------------------------------------------------
// SpurSmallInteger -- immediate integer encoded in the oop itself.
//
// Spur-64 spends 3 bits on the tag, so these are 61-bit: the range is
// [-2^60, 2^60 - 1], matching SmallInteger minVal / maxVal in the image.
// Decoding with the wrong shift still yields plausible-looking numbers, so
// prefer value() over shifting an oop by hand.
// ---------------------------------------------------------------------------
class SpurSmallInteger : public SpurObject {
public:
    SpurSmallInteger() = default;
    SpurSmallInteger(uint64_t oop, const SpurImage* image) : SpurObject(oop, image) {}

    /** Returns the decoded value, which is meaningful only when the oop has the
     *  SmallInteger tag; valueOr() handles slots that may hold anything else. **/
    int64_t value() const { return Spur::smallIntegerValue(rawOop); }

    /** Returns value() when this is a SmallInteger, otherwise `fallback`. **/
    int64_t valueOr(int64_t fallback) const {
        return isSmallInteger() ? value() : fallback;
    }

    static constexpr int64_t MAX_VALUE = (int64_t{1} << 60) - 1;
    static constexpr int64_t MIN_VALUE = -(int64_t{1} << 60);

    static bool fits(int64_t value) {
        return value >= MIN_VALUE && value <= MAX_VALUE;
    }

    static uint64_t encode(int64_t value) {
        return (static_cast<uint64_t>(value) << 3) | Spur::TAG_SMALL_INTEGER;
    }
};

// ---------------------------------------------------------------------------
// SpurCharacter -- immediate character, holding a Unicode code point.
// ---------------------------------------------------------------------------
class SpurCharacter : public SpurObject {
public:
    SpurCharacter() = default;
    SpurCharacter(uint64_t oop, const SpurImage* image) : SpurObject(oop, image) {}

    /** Returns the code point, which is meaningful only when the oop has the Character tag. **/
    uint32_t value() const { return Spur::characterValue(rawOop); }

    uint32_t valueOr(uint32_t fallback) const {
        return isCharacter() ? value() : fallback;
    }

    /** Returns the code point encoded as UTF-8, or an empty string when it is not valid. **/
    std::string asUtf8() const;

    static constexpr uint32_t MAX_CODE_POINT = 0x10FFFF;
    static bool isValidCodePoint(uint32_t codePoint) {
        return codePoint <= MAX_CODE_POINT &&
               !(codePoint >= 0xD800 && codePoint <= 0xDFFF);
    }

    static uint64_t encode(uint32_t codePoint) {
        return (static_cast<uint64_t>(codePoint) << 3) | Spur::TAG_CHARACTER;
    }
};

// ---------------------------------------------------------------------------
// SpurSmallFloat -- immediate double, for the exponent range that fits.
// ---------------------------------------------------------------------------
class SpurSmallFloat : public SpurObject {
public:
    SpurSmallFloat() = default;
    SpurSmallFloat(uint64_t oop, const SpurImage* image) : SpurObject(oop, image) {}

    /** Returns the decoded value, which is meaningful only when the oop has the
     *  SmallFloat tag. **/
    double value() const { return Spur::smallFloatValue(rawOop); }

    double valueOr(double fallback) const {
        return isSmallFloat() ? value() : fallback;
    }

    static uint64_t encode(double value) { return Spur::encodeSmallFloat(value); }
    static bool fits(double value) { return Spur::fitsSmallFloat(value); }
};

// ---------------------------------------------------------------------------
// Inline down-casts for the immediates (the SpurHeapObject one lives in
// SpurHeapObject.h, which needs the full definition).
// ---------------------------------------------------------------------------
inline SpurSmallInteger SpurObject::asSmallInteger() const {
    return SpurSmallInteger(rawOop, spurImage);
}

inline SpurCharacter SpurObject::asCharacter() const {
    return SpurCharacter(rawOop, spurImage);
}

inline SpurSmallFloat SpurObject::asSmallFloat() const {
    return SpurSmallFloat(rawOop, spurImage);
}

} // namespace osvm
