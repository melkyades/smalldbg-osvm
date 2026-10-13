#include "SpurMethodMap.h"

#include <set>

namespace osvm {

namespace {

// The JIT compiles these special selectors in place, with no send to fall back on.
const std::set<std::string> sendlessSelectors = {"==", "~~", "class"};

bool contains(const SistaV1Disassembler::Closure& block, int pc) {
    return pc >= block.start && pc < block.end;
}

} // namespace

SpurMethodMap::SpurMethodMap(const SpurCompiledMethod& method)
    : code(decodeMethod(method)), text(method.sourceCode()),
      tree(smalltalk::parseMethodSyntax(text)), numArgs(method.header().numArgs) {
    orderCompiledSends();
    matchSources();
    matchBlocks();
}

std::optional<size_t> SpurMethodMap::bytecodeAt(int pc) const {
    for (size_t i = 0; i < code.bytecodes.size(); i++) {
        const auto& bytecode = code.bytecodes[i];
        if (pc >= bytecode.pc && pc < bytecode.pc + static_cast<int>(bytecode.bytes.size()))
            return i;
    }
    return std::nullopt;
}

bool SpurMethodMap::isAtEnd(int pc, int blockStart) const {
    if (code.bytecodes.empty()) return false;
    if (blockStart == 0) {
        const auto& last = code.bytecodes.back();
        return pc >= last.pc + static_cast<int>(last.bytes.size()) - 1;
    }
    for (const auto& block : code.closures)
        if (block.start == blockStart) return pc == block.end - 1;
    return false;
}

// The JIT compiles the method's own bytecodes first, then each block's in the
// order the blocks are created, each leaving out the blocks inside it.
void SpurMethodMap::orderCompiledSends() {
    const auto& blocks = code.closures;
    const auto innermost = [&](int pc) {
        int found = -1;
        for (size_t b = 0; b < blocks.size(); b++)
            if (contains(blocks[b], pc)) found = static_cast<int>(b);
        return found;
    };
    for (int owner = -1; owner < static_cast<int>(blocks.size()); owner++) {
        for (size_t i = 0; i < code.bytecodes.size(); i++) {
            const auto& bytecode = code.bytecodes[i];
            if (bytecode.selector.empty() || sendlessSelectors.count(bytecode.selector)) continue;
            if (innermost(bytecode.pc) == owner) compiledSends.push_back(i);
        }
    }
}

std::optional<size_t> SpurMethodMap::compiledSend(size_t index, size_t count) const {
    if (count != compiledSends.size() || index >= compiledSends.size()) return std::nullopt;
    return compiledSends[index];
}

void SpurMethodMap::matchSources() {
    sources.assign(code.bytecodes.size(), std::nullopt);
    const std::vector<SourceSend> sends = squeakSendOrder(tree, text);
    std::vector<size_t> sendBytecodes;
    for (size_t i = 0; i < code.bytecodes.size(); i++)
        if (!code.bytecodes[i].selector.empty()) sendBytecodes.push_back(i);
    if (sends.size() != sendBytecodes.size()) return;
    for (size_t i = 0; i < sends.size(); i++)
        if (sends[i].selector != code.bytecodes[sendBytecodes[i]].selector) return;
    for (size_t i = 0; i < sends.size(); i++) sources[sendBytecodes[i]] = sends[i];
}

std::optional<SourceSend> SpurMethodMap::sourceOf(size_t index) const {
    return index < sources.size() ? sources[index] : std::nullopt;
}

void SpurMethodMap::matchBlocks() {
    const std::vector<int> nodes = closureBlocks(tree);
    if (nodes.size() != code.closures.size()) return;
    for (size_t i = 0; i < nodes.size(); i++)
        if (tree.at(nodes[i]).argumentNames.size() != static_cast<size_t>(code.closures[i].numArgs))
            return;
    blockNodes = nodes;
}

std::vector<std::string> SpurMethodMap::methodNames(size_t temporaries) const {
    std::vector<std::string> names;
    if (tree.argumentNames.size() != numArgs) return names;
    names = tree.argumentNames;
    std::vector<std::string> locals = tree.temporaryNames;
    const std::vector<std::string> inlined = inlinedBlockNames(tree, -1);
    locals.insert(locals.end(), inlined.begin(), inlined.end());
    if (names.size() + locals.size() == temporaries)
        names.insert(names.end(), locals.begin(), locals.end());
    return names;
}

std::vector<std::string> SpurMethodMap::blockNames(int start) const {
    std::vector<std::string> names;
    for (size_t i = 0; i < blockNodes.size(); i++) {
        const auto& block = code.closures[i];
        if (block.start != start) continue;
        const smalltalk::SyntaxNode& node = tree.at(blockNodes[i]);
        names = node.argumentNames;
        for (int copied = 1; copied <= block.numCopied; copied++)
            names.push_back("copied" + std::to_string(copied));
        names.insert(names.end(), node.temporaryNames.begin(), node.temporaryNames.end());
        const std::vector<std::string> inlined = inlinedBlockNames(tree, blockNodes[i]);
        names.insert(names.end(), inlined.begin(), inlined.end());
        break;
    }
    return names;
}

} // namespace osvm
