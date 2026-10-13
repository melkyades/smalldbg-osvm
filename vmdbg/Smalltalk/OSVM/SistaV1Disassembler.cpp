#include "SistaV1Disassembler.h"
#include "SpurImage.h"

namespace osvm {

namespace {

const char* const arithmeticSelectors[] = {
    "+", "-", "<", ">", "<=", ">=", "=", "~=",
    "*", "/", "\\\\", "@", "bitShift:", "//", "bitAnd:", "bitOr:"};
const int arithmeticArgs = 1;

const char* const specialSelectors[] = {
    "at:", "at:put:", "size", "next", "nextPut:", "atEnd", "==", "class",
    "~~", "value", "value:", "do:", "new", "new:", "x", "y"};
const int specialArgs[] = {1, 2, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0};

const char* const constants[] = {"self", "true", "false", "nil", "0", "1"};
const char* const returns[] = {"self", "true", "false", "nil"};

// Returns the index a two-byte bytecode names, its A extension giving the high bits.
int64_t extendedIndex(int operand, int64_t extA) { return operand + extA * 256; }

} // namespace

SistaV1Disassembler::SistaV1Disassembler(const std::vector<uint8_t>& bytecodes, int initialPC,
                                         LiteralText literalText, LiteralText selector)
    : code(bytecodes), firstPC(initialPC), literal(std::move(literalText)),
      selectorText(std::move(selector)) {}

int SistaV1Disassembler::byte() {
    return pos < code.size() ? code[pos++] : 0;
}

std::vector<smalltalk::Bytecode> SistaV1Disassembler::disassemble() {
    std::vector<smalltalk::Bytecode> found;
    blocks.clear();
    pos = 0;
    extA = extB = 0;
    while (pos < code.size()) {
        smalltalk::Bytecode bytecode;
        const size_t start = pos;
        bytecode.pc = firstPC + static_cast<int>(start);
        const int op = byte();
        bytecode.text = decode(op, bytecode.pc, bytecode);
        bytecode.bytes.assign(code.begin() + static_cast<long>(start),
                              code.begin() + static_cast<long>(pos));
        // An extension belongs to the bytecode it prefixes and is spent by it.
        if (op != 224 && op != 225) extA = extB = 0;
        found.push_back(std::move(bytecode));
    }
    return found;
}

std::string SistaV1Disassembler::decode(int op, int pc, smalltalk::Bytecode& out) {
    if (op < 224) return decodeOneByte(op, out);
    if (op < 248) return decodeTwoBytes(op, out);
    return decodeThreeBytes(op, pc, out);
}

std::string SistaV1Disassembler::send(const std::string& selector, int numArgs,
                                      smalltalk::Bytecode& out, const char* kind) {
    out.selector = selector;
    return std::string(kind) + " #" + selector + " (" + std::to_string(numArgs) +
           (numArgs == 1 ? " arg)" : " args)");
}

std::string SistaV1Disassembler::jump(const char* kind, int64_t distance,
                                      smalltalk::Bytecode& out) {
    const int target = firstPC + static_cast<int>(pos) + static_cast<int>(distance);
    out.target = target;
    return std::string(kind) + " " + std::to_string(target);
}

std::string SistaV1Disassembler::decodeOneByte(int op, smalltalk::Bytecode& out) {
    if (op < 16) return "pushRcvr: " + std::to_string(op);
    if (op < 32) return "pushLitVar: " + literal(op - 16);
    if (op < 64) return "pushConstant: " + literal(op - 32);
    if (op < 76) return "pushTemp: " + std::to_string(op - 64);
    if (op < 82) return std::string(op == 76 ? "push: " : "pushConstant: ") + constants[op - 76];
    if (op == 82) return extB == 1 ? "push: thisProcess" : "push: thisContext";
    if (op == 83) return "dup";
    if (op < 88) return "unassigned";
    if (op < 92) return std::string("return: ") + returns[op - 88];
    if (op == 92) return "returnTop";
    if (op == 93) return "blockReturn: nil";
    if (op == 94) return "blockReturnTop";
    if (op == 95) return "nop";
    if (op < 112) return send(arithmeticSelectors[op - 96], arithmeticArgs, out);
    if (op < 128) return send(specialSelectors[op - 112], specialArgs[op - 112], out);
    if (op < 176) return send(selectorText((op - 128) % 16), (op - 128) / 16, out);
    if (op < 184) return jump("jumpTo:", op - 176 + 1, out);
    if (op < 192) return jump("jumpTrue:", op - 184 + 1, out);
    if (op < 200) return jump("jumpFalse:", op - 192 + 1, out);
    if (op < 208) return "popIntoRcvr: " + std::to_string(op - 200);
    if (op < 216) return "popIntoTemp: " + std::to_string(op - 208);
    if (op == 216) return "pop";
    if (op == 217) return "trap";
    return "unassigned";
}

std::string SistaV1Disassembler::decodeTwoBytes(int op, smalltalk::Bytecode& out) {
    const int operand = byte();
    switch (op) {
        case 224: extA = extA * 256 + operand; return "extA: " + std::to_string(operand);
        case 225: extB = extB * 256 + static_cast<int8_t>(operand);
                  return "extB: " + std::to_string(static_cast<int8_t>(operand));
        case 226: return "pushRcvr: " + std::to_string(extendedIndex(operand, extA));
        case 227: return "pushLitVar: " + literal(static_cast<int>(extendedIndex(operand, extA)));
        case 228: return "pushConstant: " + literal(static_cast<int>(extendedIndex(operand, extA)));
        case 229: return "pushTemp: " + std::to_string(operand);
        case 231: return ((operand & 0x80) ? "pop into new Array: " : "push new Array: ") +
                         std::to_string(operand & 0x7F);
        case 232: return "pushConstant: " + std::to_string(operand + extB * 256);
        case 233: return "pushConstant: $" + std::string(1, static_cast<char>(operand + extA * 256));
        case 234:
        case 235: {
            const int index = static_cast<int>((operand >> 3) + extA * 32);
            const int numArgs = static_cast<int>((operand & 7) + (extB & 63) * 8);
            return send(selectorText(index), numArgs, out, op == 234 ? "send" : "superSend");
        }
        case 237: return jump("jumpTo:", operand + extB * 256, out);
        case 238: return jump("jumpTrue:", operand + extB * 256, out);
        case 239: return jump("jumpFalse:", operand + extB * 256, out);
        case 240: return "popIntoRcvr: " + std::to_string(extendedIndex(operand, extA));
        case 241: return "popIntoLitVar: " + literal(static_cast<int>(extendedIndex(operand, extA)));
        case 242: return "popIntoTemp: " + std::to_string(operand);
        case 243: return "storeIntoRcvr: " + std::to_string(extendedIndex(operand, extA));
        case 244: return "storeIntoLitVar: " + literal(static_cast<int>(extendedIndex(operand, extA)));
        case 245: return "storeIntoTemp: " + std::to_string(operand);
        default:  return "unassigned";
    }
}

std::string SistaV1Disassembler::decodeThreeBytes(int op, int pc, smalltalk::Bytecode& out) {
    const int first = byte();
    const int second = byte();
    switch (op) {
        case 248: return "callPrimitive: " + std::to_string(first + (second & 0x1F) * 256);
        case 249: return "pushFullClosure: " + literal(static_cast<int>(first + extA * 256)) +
                         " numCopied: " + std::to_string(second & 0x3F);
        case 250: {
            Closure block;
            block.pc = pc;
            block.numCopied = ((first >> 3) & 7) + static_cast<int>(extA / 16) * 8;
            block.numArgs = (first & 7) + static_cast<int>(extA % 16) * 8;
            block.start = firstPC + static_cast<int>(pos);
            block.end = block.start + second + static_cast<int>(extB) * 256;
            blocks.push_back(block);
            out.target = block.end;
            return "closure numCopied: " + std::to_string(block.numCopied) + " numArgs: " +
                   std::to_string(block.numArgs) + " to: " + std::to_string(block.end);
        }
        case 251: return (second & 0x80 ? "pushRcvr: " : "pushTemp: ") + std::to_string(first) +
                         " inVectorAt: " + std::to_string(second & 0x7F);
        case 252: return (second & 0x80 ? "storeIntoRcvr: " : "storeIntoTemp: ") + std::to_string(first) +
                         " inVectorAt: " + std::to_string(second & 0x7F);
        case 253: return (second & 0x80 ? "popIntoRcvr: " : "popIntoTemp: ") + std::to_string(first) +
                         " inVectorAt: " + std::to_string(second & 0x7F);
        default:  return "unassigned";
    }
}

MethodCode decodeMethod(const SpurCompiledMethod& method) {
    if (!method || !SistaV1Disassembler::usesSet(method.headerValue())) return {};
    const SpurImage* image = method.image();
    const std::vector<SpurObject> literals = method.literals();
    const auto literalAt = [&](int index) -> uint64_t {
        return index >= 0 && static_cast<size_t>(index) < literals.size()
            ? literals[static_cast<size_t>(index)].oop() : 0;
    };
    const std::vector<uint8_t> bytes = method.bytecodes();
    SistaV1Disassembler disassembler(
        bytes, method.initialPC(),
        [&](int index) { return image->printStringOf(literalAt(index)); },
        [&](int index) { return image->stringOf(literalAt(index)); });
    MethodCode code;
    code.bytecodes = disassembler.disassemble();
    code.closures = disassembler.closures();
    return code;
}

} // namespace osvm
