#pragma once

#include <cstdint>
#include <cstddef>

namespace osvm {

/** Spur describes the 64-bit Spur object format. Every object has one 8-byte header word:
 *
 *    bits 0..21   classIndex   (index into the class table)
 *    bits 24..28  format       (instSpec, see Fmt below)
 *    bits 32..53  identityHash
 *    bits 56..63  numSlots     (255 means the real count is in the
 *                               8-byte word immediately before the header)
 *
 *  An oop points at the header word; slot 0 starts 8 bytes later. Immediates
 *  use the low 3 bits as a tag, so SmallIntegers have 61 bits. **/
struct Spur {
    static constexpr uint64_t CLASS_INDEX_MASK = 0x3FFFFF;
    /** SEGMENT_BRIDGE is the class index of the object that ends an old space
     *  segment. It spans the gap to the next segment, and has no slots in the last one. **/
    static constexpr uint32_t SEGMENT_BRIDGE   = 3;
    static constexpr int      FORMAT_SHIFT     = 24;
    static constexpr uint64_t FORMAT_MASK      = 0x1F;
    static constexpr int      HASH_SHIFT       = 32;
    static constexpr uint64_t HASH_MASK        = 0x3FFFFF;
    static constexpr int      SLOTS_SHIFT      = 56;
    static constexpr uint64_t SLOTS_OVERFLOW   = 0xFF;
    static constexpr uint64_t OVERFLOW_MASK    = 0x00FFFFFFFFFFFFFFull;

    static constexpr uint64_t BASE_HEADER_SIZE = 8;
    static constexpr uint64_t BYTES_PER_OOP    = 8;

    static constexpr uint64_t TAG_MASK          = 7;
    static constexpr uint64_t TAG_SMALL_INTEGER = 1;
    static constexpr uint64_t TAG_CHARACTER     = 2;
    static constexpr uint64_t TAG_SMALL_FLOAT   = 4;

    static constexpr uint32_t CLASS_TABLE_PAGE_SHIFT = 10;
    static constexpr uint32_t CLASS_TABLE_PAGE_MASK  = 0x3FF;

    static bool isImmediate(uint64_t oop)    { return (oop & TAG_MASK) != 0; }
    static bool isSmallInteger(uint64_t oop) { return (oop & TAG_SMALL_INTEGER) != 0; }
    static bool isCharacter(uint64_t oop)    { return (oop & TAG_MASK) == TAG_CHARACTER; }
    static bool isSmallFloat(uint64_t oop)   { return (oop & TAG_MASK) == TAG_SMALL_FLOAT; }

    static int64_t smallIntegerValue(uint64_t oop) {
        return static_cast<int64_t>(oop) >> 3;
    }
    static uint32_t characterValue(uint64_t oop) {
        return static_cast<uint32_t>(oop >> 3);
    }

    // ---- SmallFloat64 ----
    //
    // A double keeps its 52-bit mantissa but only 8 exponent bits, which is
    // what makes 1 + 8 + 52 fit beside the 3 tag bits. The payload is rotated
    // so the sign lands in its lowest bit, and the exponent is stored biased
    // down by this offset. An exponent field of 0 means zero or a denormal
    // and is left alone.
    static constexpr uint64_t SMALL_FLOAT_EXPONENT_OFFSET = 896;
    static constexpr uint64_t DOUBLE_MANTISSA_MASK = 0xFFFFFFFFFFFFFull;
    static constexpr uint64_t DOUBLE_EXPONENT_MASK = 0x7FF;
    static constexpr int      DOUBLE_EXPONENT_SHIFT = 52;
    static constexpr uint64_t SMALL_FLOAT_EXPONENT_MASK = 0xFF;

    union DoubleBits {
        uint64_t bits;
        double value;
    };

    static double smallFloatValue(uint64_t oop) {
        const uint64_t payload = oop >> 3;
        const uint64_t sign = payload & 1;
        const uint64_t rest = payload >> 1;
        uint64_t exponent = (rest >> DOUBLE_EXPONENT_SHIFT) & SMALL_FLOAT_EXPONENT_MASK;
        const uint64_t mantissa = rest & DOUBLE_MANTISSA_MASK;
        if (exponent != 0) exponent += SMALL_FLOAT_EXPONENT_OFFSET;

        DoubleBits punned;
        punned.bits = (sign << 63) | (exponent << DOUBLE_EXPONENT_SHIFT) | mantissa;
        return punned.value;
    }

    static uint64_t encodeSmallFloat(double value) {
        DoubleBits punned;
        punned.value = value;
        const uint64_t sign = punned.bits >> 63;
        uint64_t exponent = (punned.bits >> DOUBLE_EXPONENT_SHIFT) & DOUBLE_EXPONENT_MASK;
        const uint64_t mantissa = punned.bits & DOUBLE_MANTISSA_MASK;
        if (exponent != 0) exponent -= SMALL_FLOAT_EXPONENT_OFFSET;

        const uint64_t payload =
            ((((exponent << DOUBLE_EXPONENT_SHIFT) | mantissa) << 1) | sign);
        return (payload << 3) | TAG_SMALL_FLOAT;
    }

    /** Returns false for doubles outside the 8-bit exponent range and for every infinity
     *  and NaN, which have to stay boxed. **/
    static bool fitsSmallFloat(double value) {
        DoubleBits punned;
        punned.value = value;
        const uint64_t exponent = (punned.bits >> DOUBLE_EXPONENT_SHIFT) & DOUBLE_EXPONENT_MASK;
        if (exponent == 0) return true;
        const int64_t reduced = static_cast<int64_t>(exponent) -
                                static_cast<int64_t>(SMALL_FLOAT_EXPONENT_OFFSET);
        return reduced >= 1 && reduced <= static_cast<int64_t>(SMALL_FLOAT_EXPONENT_MASK);
    }

    /** Returns the class table index of an immediate, which the VM assigns by tag. **/
    static uint32_t immediateClassIndex(uint64_t oop) {
        if (isSmallInteger(oop)) return 1;
        return (oop & TAG_MASK) == TAG_CHARACTER ? 2 : 4;
    }

    /** Returns the bytes an object of `slots` slots occupies, header included. An empty
     *  object still takes one slot. **/
    static uint64_t objectBytesForSlots(uint64_t slots) {
        return slots == 0 ? BASE_HEADER_SIZE + BYTES_PER_OOP
                          : BASE_HEADER_SIZE + slots * BYTES_PER_OOP;
    }
};

/** Fmt lists the instSpec values of the `format` header field. **/
struct Fmt {
    static constexpr uint32_t ZERO_SIZED      = 0;
    static constexpr uint32_t FIXED           = 1;
    static constexpr uint32_t VARIABLE        = 2;
    static constexpr uint32_t FIXED_VARIABLE  = 3;
    static constexpr uint32_t WEAK_VARIABLE   = 4;
    static constexpr uint32_t WEAK_FIXED      = 5;
    static constexpr uint32_t INDEXABLE_64    = 9;
    static constexpr uint32_t INDEXABLE_32    = 10;
    static constexpr uint32_t INDEXABLE_16    = 12;
    static constexpr uint32_t INDEXABLE_8     = 16;
    static constexpr uint32_t COMPILED_METHOD = 24;

    static bool isPointers(uint32_t f) { return f <= WEAK_FIXED; }
    static bool isBytes(uint32_t f)    { return f >= INDEXABLE_8 && f < COMPILED_METHOD; }
    static bool isMethod(uint32_t f)   { return f >= COMPILED_METHOD; }
    /** Returns the count of unused trailing bytes, which byte objects keep in the low
     *  format bits. **/
    static uint32_t oddBytes(uint32_t f) { return f & 7; }
};

/** BehaviorFormat decodes the `format` instance variable of a Behavior: instSpec in the
 *  high bits, fixed field count in the low 16. **/
struct BehaviorFormat {
    static constexpr int      INST_SPEC_SHIFT = 16;
    static constexpr uint64_t INST_SPEC_MASK  = 0x1F;
    static constexpr uint64_t FIXED_MASK      = 0xFFFF;

    static uint32_t instSpec(int64_t f) {
        return static_cast<uint32_t>((f >> INST_SPEC_SHIFT) & INST_SPEC_MASK);
    }
    static uint32_t fixedFields(int64_t f) {
        return static_cast<uint32_t>(f & FIXED_MASK);
    }
};

/** SpecialObject lists well-known indices into the special objects array. **/
struct SpecialObject {
    static constexpr uint64_t NIL              = 0;
    static constexpr uint64_t FALSE            = 1;
    static constexpr uint64_t TRUE             = 2;
    static constexpr uint64_t SCHEDULER_ASSOC  = 3;
    static constexpr uint64_t SMALLTALK        = 8;
    static constexpr uint64_t COMPILED_METHOD  = 16;
};

} // namespace osvm
