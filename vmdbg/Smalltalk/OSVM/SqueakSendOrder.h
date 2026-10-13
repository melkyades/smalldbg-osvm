#pragma once

#include "Smalltalk/SyntaxTree.h"
#include <cstddef>
#include <string>
#include <vector>

namespace osvm {

/** A SourceSend is a message a method sends and where its source shows it. **/
struct SourceSend {
    std::string selector;
    size_t start{0};
    size_t end{0};
};

/** Returns the sends a Squeak-family compiler emits for a method, in bytecode order,
 *  including the sends of the blocks it compiles in place. `source` is the text the
 *  tree was parsed from. **/
std::vector<SourceSend> squeakSendOrder(const smalltalk::SyntaxTree& tree,
                                        const std::string& source);

/** Returns the blocks of a method that become closures rather than jumps, as node
 *  indices in the order the compiler creates them. **/
std::vector<int> closureBlocks(const smalltalk::SyntaxTree& tree);

/** Returns the arguments and temporaries the blocks inlined into `scope` declare, in
 *  source order. `scope` is a block node, or -1 for the method. **/
std::vector<std::string> inlinedBlockNames(const smalltalk::SyntaxTree& tree, int scope);

} // namespace osvm
