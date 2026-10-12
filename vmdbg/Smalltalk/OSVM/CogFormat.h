#pragma once

#include <cstdint>

namespace osvm {

/** Cog holds where a 64-bit Cog VM keeps a process's activations: the frame
 *  layout of CoInterpreter's stack pages and the fields of a compiled method
 *  that frames point at. Offsets are in bytes. **/
struct Cog {
    // Relative to a frame pointer. Above it are the caller's saved ip, then the
    // arguments, then the receiver or, for a block, the closure.
    static constexpr int64_t SAVED_FP        = 0;
    static constexpr int64_t CALLER_SAVED_IP = 8;
    static constexpr int64_t METHOD          = -8;
    static constexpr int64_t THIS_CONTEXT    = -16;
    static constexpr int64_t IFRAME_FLAGS    = -24;
    static constexpr int64_t IFRAME_SAVED_IP = -32;
    static constexpr int64_t IFRAME_RECEIVER = -40;
    static constexpr int64_t MFRAME_RECEIVER = -24;

    // The bytes of an interpreter frame's flags word.
    static constexpr int IFRAME_NUM_ARGS_BYTE    = 1;
    static constexpr int IFRAME_HAS_CONTEXT_BYTE = 2;
    static constexpr int IFRAME_IS_BLOCK_BYTE    = 3;

    // A machine code frame's method field is a CogMethod address and flags.
    static constexpr uint64_t MFRAME_HAS_CONTEXT = 1;
    static constexpr uint64_t MFRAME_IS_BLOCK    = 2;
    static constexpr uint64_t MFRAME_FLAGS_MASK  = 7;

    // CogMethod and CogBlockMethod fields. A block's code is part of its home
    // method's, homeOffset bytes in, unless it is a full block of its own.
    static constexpr uint64_t COG_HOME_OFFSET   = 0;
    static constexpr uint64_t COG_FLAGS         = 8;
    static constexpr uint64_t COG_METHOD_OBJECT = 16;
    static constexpr uint64_t COG_METHOD_HEADER = 24;
    static constexpr uint64_t COG_METHOD_SIZE   = 40;  // the code follows
    static constexpr uint64_t COG_TYPE_SHIFT    = 8;
    static constexpr uint64_t COG_TYPE_MASK     = 7;
    static constexpr uint64_t COG_NUM_ARGS_MASK = 0xFF;
    static constexpr uint64_t COG_HOME_MASK     = 0xFFFF;
    static constexpr uint64_t CM_METHOD         = 5;
};

} // namespace osvm
