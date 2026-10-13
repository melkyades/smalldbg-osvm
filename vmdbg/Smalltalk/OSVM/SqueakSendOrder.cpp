#include "SqueakSendOrder.h"

#include <set>

namespace osvm {

namespace {

using smalltalk::SyntaxNode;
using smalltalk::SyntaxTree;

// The messages MessageNode compiles to jumps when their blocks are literal.
const std::set<std::string> inlinedSelectors = {
    "ifTrue:", "ifFalse:", "ifTrue:ifFalse:", "ifFalse:ifTrue:", "and:", "or:",
    "whileFalse:", "whileTrue:", "whileFalse", "whileTrue", "to:do:", "to:by:do:",
    "ifNil:", "ifNotNil:", "ifNil:ifNotNil:", "ifNotNil:ifNil:", "repeat",
};

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

bool isBlock(const SyntaxTree& tree, int index) {
    return index >= 0 && tree.at(index).kind == SyntaxNode::Kind::Block;
}

// Returns true when the compiler inlines the send, which needs the bodies of
// its loops and conditionals to be literal blocks; to:do: takes any limit.
bool isInlined(const SyntaxTree& tree, const SyntaxNode& node) {
    if (node.kind != SyntaxNode::Kind::Send || inlinedSelectors.count(node.selector) == 0)
        return false;
    if (node.arguments.empty()) return isBlock(tree, node.receiver);
    if (startsWith(node.selector, "while") && !isBlock(tree, node.receiver)) return false;
    if (startsWith(node.selector, "to:")) return isBlock(tree, node.arguments.back());
    for (int argument : node.arguments)
        if (!isBlock(tree, argument)) return false;
    return true;
}

class SendOrder {
public:
    SendOrder(const SyntaxTree& syntax, const std::string& text) : tree(syntax), source(text) {}

    std::vector<SourceSend> run() {
        emitAll(tree.statements);
        return std::move(sends);
    }

private:
    const SyntaxTree& tree;
    const std::string& source;
    std::vector<SourceSend> sends;

    void emitAll(const std::vector<int>& nodes) {
        for (int node : nodes) emit(node);
    }

    void emit(int index) {
        if (index < 0) return;
        const SyntaxNode& node = tree.at(index);
        switch (node.kind) {
            case SyntaxNode::Kind::Assignment: emit(node.receiver); return;
            case SyntaxNode::Kind::Block:
            case SyntaxNode::Kind::Brace: emitAll(node.children); return;
            case SyntaxNode::Kind::Cascade:
                emit(node.receiver);
                for (int message : node.children) emitSend(tree.at(message), true);
                return;
            case SyntaxNode::Kind::Send: emitSend(node, false); return;
            case SyntaxNode::Kind::Leaf: return;
        }
    }

    void add(const std::string& selector, const SyntaxNode& node) {
        sends.push_back({selector, node.start, node.end});
    }

    // Emits the sends of an inlined message: its receiver and blocks in order,
    // an == for a nil test, and for a counted loop the test before the body and
    // the step after it, the test reversed when the step is negative.
    void emitInlined(const SyntaxNode& node, bool receiverDone) {
        if (startsWith(node.selector, "while")) {
            emit(node.receiver);
            emitAll(node.arguments);
            return;
        }
        if (!receiverDone) emit(node.receiver);
        if (startsWith(node.selector, "ifNil") || startsWith(node.selector, "ifNotNil"))
            add("==", node);
        if (startsWith(node.selector, "to:")) {
            emit(node.arguments.front());
            add(countsDown(node) ? ">=" : "<=", node);
            emit(node.arguments.back());
            add("+", node);
            return;
        }
        emitAll(node.arguments);
    }

    bool countsDown(const SyntaxNode& node) const {
        if (node.selector != "to:by:do:") return false;
        const SyntaxNode& step = tree.at(node.arguments[1]);
        return step.kind == SyntaxNode::Kind::Leaf && step.start > 0 &&
               source.size() > step.start && source[step.start - 1] == '-';
    }

    void emitSend(const SyntaxNode& node, bool receiverDone) {
        if (isInlined(tree, node)) { emitInlined(node, receiverDone); return; }
        if (!receiverDone) emit(node.receiver);
        emitAll(node.arguments);
        add(node.selector, node);
    }
};

// Walks the nodes under `index`, calling `visit` with each block and whether
// a send inlines it.
template <typename Visit>
void blocksDo(const SyntaxTree& tree, int index, bool inlined, Visit&& visit) {
    if (index < 0) return;
    const SyntaxNode& node = tree.at(index);
    if (node.kind == SyntaxNode::Kind::Block) visit(index, inlined);
    if (node.kind == SyntaxNode::Kind::Send) {
        const bool inlines = isInlined(tree, node);
        blocksDo(tree, node.receiver, inlines && isBlock(tree, node.receiver), visit);
        for (int argument : node.arguments)
            blocksDo(tree, argument, inlines && isBlock(tree, argument), visit);
        return;
    }
    if (node.kind == SyntaxNode::Kind::Cascade) {
        blocksDo(tree, node.receiver, false, visit);
        for (int message : node.children) blocksDo(tree, message, false, visit);
        return;
    }
    if (node.kind == SyntaxNode::Kind::Assignment) blocksDo(tree, node.receiver, false, visit);
    for (int child : node.children) blocksDo(tree, child, false, visit);
}

} // namespace

std::vector<SourceSend> squeakSendOrder(const SyntaxTree& tree, const std::string& source) {
    return SendOrder(tree, source).run();
}

std::vector<int> closureBlocks(const SyntaxTree& tree) {
    std::vector<int> found;
    for (int statement : tree.statements)
        blocksDo(tree, statement, false, [&](int block, bool inlined) {
            if (!inlined) found.push_back(block);
        });
    return found;
}

// Only blocks inlined straight into the scope count; a closure inside it has
// its own temporaries, and so do the blocks inlined into that closure.
std::vector<std::string> inlinedBlockNames(const SyntaxTree& tree, int scope) {
    std::vector<std::string> names;
    std::set<int> closures;
    const std::vector<int>& top = scope < 0 ? tree.statements : tree.at(scope).children;
    for (int statement : top)
        blocksDo(tree, statement, false, [&](int block, bool inlined) {
            if (!inlined) { closures.insert(block); return; }
            for (int closure : closures) {
                const SyntaxNode& outer = tree.at(closure);
                if (tree.at(block).start > outer.start && tree.at(block).end <= outer.end) return;
            }
            const SyntaxNode& node = tree.at(block);
            names.insert(names.end(), node.argumentNames.begin(), node.argumentNames.end());
            names.insert(names.end(), node.temporaryNames.begin(), node.temporaryNames.end());
        });
    return names;
}

} // namespace osvm
