#pragma once

#include "SistaV1Disassembler.h"
#include "SpurKnownObjects.h"
#include "SqueakSendOrder.h"
#include "Smalltalk/SyntaxTree.h"
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace osvm {

/** A SpurMethodMap places activations of one method: the bytecode a pc or a send its
 *  machine code makes is at, the source each send came from, and the names of the
 *  method's and its blocks' temporaries. A match with the source is used only when
 *  the source agrees with the bytecodes the compiler made from it. **/
class SpurMethodMap {
public:
    explicit SpurMethodMap(const SpurCompiledMethod& method);

    const std::string& source() const { return text; }
    const std::vector<smalltalk::Bytecode>& bytecodes() const { return code.bytecodes; }

    /** Returns the index of the bytecode that holds the byte at `pc`, or nothing when
     *  no bytecode does. **/
    std::optional<size_t> bytecodeAt(int pc) const;
    /** Returns true when an activation about to run `pc` has nothing left to run, as
     *  ContextPart>>isDone tests it: a method's pc is at or past its last byte, and
     *  that of the block starting at `blockStart` at the block's last byte. **/
    bool isAtEnd(int pc, int blockStart) const;
    /** Returns the bytecode of the `index`th send the JIT compiled, when the method's
     *  machine code makes `count` sends, as many as its bytecodes call for. **/
    std::optional<size_t> compiledSend(size_t index, size_t count) const;
    /** Returns the source of the send bytecode `index` makes, as byte offsets, or
     *  nothing when the source does not match the method's sends. **/
    std::optional<SourceSend> sourceOf(size_t index) const;

    /** Returns the names of the method's arguments, then of its other temporaries
     *  when the source accounts for as many as `temporaries` holds. **/
    std::vector<std::string> methodNames(size_t temporaries) const;
    /** Returns the names of the block whose body starts at `start`: its arguments,
     *  then a name for each value it copied, then its own temporaries. **/
    std::vector<std::string> blockNames(int start) const;
    /** Returns true when the source accounts for every block the method creates. **/
    bool namesBlocks() const { return blockNodes.size() == code.closures.size(); }

private:
    void orderCompiledSends();
    void matchSources();
    void matchBlocks();

    MethodCode code;
    std::string text;
    smalltalk::SyntaxTree tree;
    uint32_t numArgs{0};
    std::vector<size_t> compiledSends;          // bytecode indices, in the JIT's order
    std::vector<std::optional<SourceSend>> sources;  // per bytecode
    std::vector<int> blockNodes;                // per closure, when they match
};

} // namespace osvm
