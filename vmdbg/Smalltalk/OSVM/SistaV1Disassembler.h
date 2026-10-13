#pragma once

#include "Smalltalk/Inspector.h"
#include "SpurKnownObjects.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace osvm {

/** A SistaV1Disassembler turns a CompiledMethod's bytecodes in the SistaV1 set, the one
 *  Cuis and Pharo compile to, into readable instructions. Pcs count from 1 at the
 *  method's header word, as a Context's pc does. The encoding is the one
 *  EncoderForSistaV1's class comment lists. **/
class SistaV1Disassembler {
public:
    /** A LiteralText returns how literal `index` reads, counted from 0 after the header. **/
    using LiteralText = std::function<std::string(int index)>;

    /** A Closure is a block a method creates in place, its body following the bytecode
     *  that creates it. **/
    struct Closure {
        int pc{0};          // of the bytecode creating it
        int start{0};       // first pc of its body
        int end{0};         // pc just past its body
        int numArgs{0};
        int numCopied{0};
    };

    /** Reads `bytecodes`, the method's bytecodes alone with the first at pc `initialPC`.
     *  `literal` says how a literal reads, and `selector` gives a selector's text. **/
    SistaV1Disassembler(const std::vector<uint8_t>& bytecodes, int initialPC,
                        LiteralText literal, LiteralText selector);

    std::vector<smalltalk::Bytecode> disassemble();
    /** Returns the blocks the method creates in place, in the order they appear. **/
    const std::vector<Closure>& closures() const { return blocks; }

    /** Returns true when the method header says the method is in this set. **/
    static bool usesSet(int64_t methodHeader) { return methodHeader < 0; }

private:
    const std::vector<uint8_t>& code;
    int firstPC;
    LiteralText literal;
    LiteralText selectorText;
    std::vector<Closure> blocks;
    size_t pos{0};
    int64_t extA{0};
    int64_t extB{0};

    int byte();
    std::string decode(int op, int pc, smalltalk::Bytecode& out);
    std::string decodeOneByte(int op, smalltalk::Bytecode& out);
    std::string decodeTwoBytes(int op, smalltalk::Bytecode& out);
    std::string decodeThreeBytes(int op, int pc, smalltalk::Bytecode& out);
    std::string send(const std::string& selector, int numArgs, smalltalk::Bytecode& out,
                     const char* kind = "send");
    std::string jump(const char* kind, int64_t distance, smalltalk::Bytecode& out);
};

/** MethodCode is a method's decoded bytecodes and the blocks it creates in place. **/
struct MethodCode {
    std::vector<smalltalk::Bytecode> bytecodes;
    std::vector<SistaV1Disassembler::Closure> closures;
};

/** Returns the method's code decoded, or none when it is in another bytecode set. **/
MethodCode decodeMethod(const SpurCompiledMethod& method);

} // namespace osvm
