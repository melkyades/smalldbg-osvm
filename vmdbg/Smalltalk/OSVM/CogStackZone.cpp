#include "CogStackZone.h"
#include "CogFrame.h"
#include "SpurImage.h"
#include <smalldbg/Debugger.h>
#include <smalldbg/Process.h>
#include <smalldbg/symbols/SymbolProvider.h>
#include <limits>
#include <vector>

namespace osvm {

namespace {

constexpr const char* SYM_FRAME_POINTER = "framePointer";

// The zone is a few hundred kilobytes allocated near the top of the VM thread's
// stack, well within this of the stack pointer.
constexpr uint64_t MAX_ZONE_DEPTH = 64ull << 20;

bool isWritableData(const smalldbg::ModuleSection& section) {
    return !section.executable && (section.name == ".data" || section.name == ".bss");
}

} // namespace

bool CogStackZone::readsAsStackPage(uint64_t framePointer) const {
    const std::vector<CogFrame> frames = framesFrom(framePointer, image);
    return !frames.empty() && frames.back().isBase();
}

std::shared_ptr<smalldbg::Thread> CogStackZone::threadHolding(uint64_t framePointer) const {
    smalldbg::Debugger* debugger = image->debugger();
    auto process = debugger->getProcess();
    if (!process || framePointer == 0) return nullptr;

    std::shared_ptr<smalldbg::Thread> holder;
    uint64_t nearest = MAX_ZONE_DEPTH;
    for (const auto& thread : process->threads()) {
        smalldbg::Registers regs;
        if (debugger->getRegisters(thread.get(), regs) != smalldbg::Status::Ok) continue;
        // A thread in machine code runs on the stack pages themselves, so its
        // stack pointer can sit above a frame pointer the VM saved earlier.
        const uint64_t sp = regs.sp();
        const uint64_t distance = sp > framePointer ? sp - framePointer : framePointer - sp;
        if (sp == 0 || distance >= nearest) continue;
        nearest = distance;
        holder = thread;
    }
    return holder;
}

uint64_t CogStackZone::globalAddressBySymbol() const {
    auto* symbols = image->debugger()->getSymbolProvider();
    if (symbols == nullptr) return 0;
    auto found = symbols->getSymbolByName(SYM_FRAME_POINTER);
    return found ? found->address : 0;
}

// Only words that point into some thread's stack are worth reading as frames.
// The deepest frame found is taken, since a word that still holds an older
// frame pointer points above the frames called since.
uint64_t CogStackZone::globalAddressByScan() const {
    smalldbg::Debugger* debugger = image->debugger();
    auto process = debugger->getProcess();
    auto* symbols = debugger->getSymbolProvider();
    if (!process || symbols == nullptr) return 0;

    std::vector<uint64_t> stackPointers;
    for (const auto& thread : process->threads()) {
        smalldbg::Registers regs;
        if (debugger->getRegisters(thread.get(), regs) == smalldbg::Status::Ok && regs.sp() != 0)
            stackPointers.push_back(regs.sp());
    }
    const auto inSomeStack = [&](uint64_t word) {
        for (uint64_t sp : stackPointers)
            if (word >= sp && word - sp < MAX_ZONE_DEPTH) return true;
        return false;
    };

    uint64_t best = 0;
    uint64_t bestFramePointer = std::numeric_limits<uint64_t>::max();
    for (const auto& module : symbols->getModules()) {
        for (const auto& section : debugger->moduleSections(module)) {
            if (!isWritableData(section) || section.end <= section.start) continue;
            std::vector<uint64_t> words((section.end - section.start) / sizeof(uint64_t));
            if (debugger->readMemory(section.start, words.data(),
                                     words.size() * sizeof(uint64_t)) != smalldbg::Status::Ok)
                continue;
            for (size_t i = 0; i < words.size(); i++) {
                const uint64_t word = words[i];
                if (word % sizeof(uint64_t) != 0 || !inSomeStack(word)) continue;
                if (word >= bestFramePointer || !readsAsStackPage(word)) continue;
                best = section.start + i * sizeof(uint64_t);
                bestFramePointer = word;
            }
        }
    }
    return best;
}

uint64_t CogStackZone::activeFramePointer() {
    smalldbg::Debugger* debugger = image->debugger();
    if (globalAddress == 0) globalAddress = globalAddressBySymbol();
    if (globalAddress == 0 && scannedVersion != debugger->stateVersion()) {
        scannedVersion = debugger->stateVersion();
        globalAddress = globalAddressByScan();
    }
    const uint64_t global = globalAddress != 0 ? image->memoryReader()->readWord(globalAddress) : 0;

    if (auto vmThread = threadHolding(global)) {
        smalldbg::Registers regs;
        if (debugger->getRegisters(vmThread.get(), regs) == smalldbg::Status::Ok &&
            readsAsStackPage(regs.fp()))
            return regs.fp();
    }
    return readsAsStackPage(global) ? global : 0;
}

} // namespace osvm
