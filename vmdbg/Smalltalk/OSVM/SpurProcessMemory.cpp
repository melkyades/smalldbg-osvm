#include "SpurProcessMemory.h"
#include "SpurFormat.h"
#include <smalldbg/symbols/SymbolProvider.h>
#include <cstdio>
#include <string>
#include <vector>

// The prologue scan walks the target's address space directly. A 32-bit host
// cannot express a 64-bit target's addresses, so it is built only for x64 and
// those builds fall back to the interpreter symbols.
#if defined(_WIN32) && defined(_WIN64)
#define OSVM_HAVE_PROLOGUE_SCAN 1
#include <windows.h>
#endif

namespace osvm {

namespace {

// The interpreter declares these at file scope in the generated C sources,
// so they survive in any build that keeps its data symbols.
constexpr const char* SYM_NIL          = "nilObj";
constexpr const char* SYM_FALSE        = "falseObj";
constexpr const char* SYM_TRUE         = "trueObj";
constexpr const char* SYM_HIDDEN_ROOTS = "hiddenRootsObj";
constexpr const char* SYM_SPECIAL_OOPS = "specialObjectsOop";

// Every Spur heap opens with nil, false and true: three zero-slot objects at
// 16-byte spacing whose class indices are fixed by the bootstrap.
constexpr uint32_t NIL_CLASS_INDEX = 1024;
constexpr uint64_t PROLOGUE_STRIDE = 16;
constexpr uint64_t PROLOGUE_SPAN   = 48;

constexpr size_t SCAN_CHUNK = 1u << 20;
constexpr uint64_t MAX_SCAN_BYTES = 4ull << 30;
constexpr uint64_t MIN_CLASS_TABLE_SLOTS = 4096;

std::string hexOf(uint64_t value) {
    char text[19];
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
    return text;
}

bool isZeroSlotObjectOfClass(uint64_t header, uint32_t classIndex) {
    return (header & Spur::CLASS_INDEX_MASK) == classIndex &&
           ((header >> Spur::FORMAT_SHIFT) & Spur::FORMAT_MASK) == Fmt::ZERO_SIZED &&
           ((header >> Spur::SLOTS_SHIFT) & Spur::SLOTS_OVERFLOW) == 0;
}

} // namespace

bool SpurProcessMemory::readGlobalOop(const char* symbol, uint64_t& out) const {
    auto* symbols = reader->getSymbolProvider();
    if (symbols == nullptr) return false;

    auto found = symbols->getSymbolByName(symbol);
    if (!found) return false;

    return readBytes(found->address, &out, sizeof(out));
}

bool SpurProcessMemory::rootsFromSymbols(SpurRoots& found) const {
    return readGlobalOop(SYM_NIL, found.nilObj) &&
           readGlobalOop(SYM_FALSE, found.falseObj) &&
           readGlobalOop(SYM_TRUE, found.trueObj) &&
           readGlobalOop(SYM_HIDDEN_ROOTS, found.hiddenRootsObj) &&
           readGlobalOop(SYM_SPECIAL_OOPS, found.specialObjectsOop);
}

bool SpurProcessMemory::prologueMatchesAt(uint64_t address) const {
    uint64_t headers[3];
    for (int i = 0; i < 3; i++) {
        if (!readBytes(address + i * PROLOGUE_STRIDE, &headers[i], sizeof(uint64_t)))
            return false;
    }
    return isZeroSlotObjectOfClass(headers[0], NIL_CLASS_INDEX) &&
           isZeroSlotObjectOfClass(headers[1], NIL_CLASS_INDEX + 1) &&
           isZeroSlotObjectOfClass(headers[2], NIL_CLASS_INDEX + 2);
}

uint64_t SpurProcessMemory::objectAfter(uint64_t oop) const {
    const uint64_t header = readWord(oop);
    uint64_t slots = (header >> Spur::SLOTS_SHIFT) & Spur::SLOTS_OVERFLOW;
    if (slots == Spur::SLOTS_OVERFLOW)
        slots = readWord(oop - Spur::BASE_HEADER_SIZE) & Spur::OVERFLOW_MASK;

    uint64_t next = oop + Spur::objectBytesForSlots(slots);
    if (((readWord(next) >> Spur::SLOTS_SHIFT) & Spur::SLOTS_OVERFLOW) == Spur::SLOTS_OVERFLOW)
        next += Spur::BASE_HEADER_SIZE;
    return next;
}

bool SpurProcessMemory::looksLikeClassTable(uint64_t oop) const {
    const uint64_t header = readWord(oop);
    if (((header >> Spur::FORMAT_SHIFT) & Spur::FORMAT_MASK) != Fmt::VARIABLE) return false;
    uint64_t slots = (header >> Spur::SLOTS_SHIFT) & Spur::SLOTS_OVERFLOW;
    if (slots == Spur::SLOTS_OVERFLOW)
        slots = readWord(oop - Spur::BASE_HEADER_SIZE) & Spur::OVERFLOW_MASK;
    return slots >= MIN_CLASS_TABLE_SLOTS;
}

#ifdef OSVM_HAVE_PROLOGUE_SCAN

bool SpurProcessMemory::rootsFromPrologueScan(SpurRoots& found, uint64_t imageBase,
                                              uint64_t imageSpecialObjects,
                                              std::string& why) const {
    auto pid = reader->attachedPid();
    if (!pid) {
        why = "the target has no process to scan";
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                 FALSE, static_cast<DWORD>(*pid));
    if (process == nullptr) {
        why = "OpenProcess failed for pid " + std::to_string(*pid)
            + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    uint64_t regionsWalked = 0;
    uint64_t regionsRead = 0;

    std::vector<uint8_t> buffer(SCAN_CHUNK + PROLOGUE_SPAN);
    uint64_t scanned = 0;
    uint64_t address = 0;
    uint64_t heapBase = 0;
    MEMORY_BASIC_INFORMATION info;

    while (heapBase == 0 && scanned < MAX_SCAN_BYTES &&
           VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != 0) {
        const uint64_t regionBase = reinterpret_cast<uint64_t>(info.BaseAddress);
        const uint64_t regionSize = static_cast<uint64_t>(info.RegionSize);
        regionsWalked++;

        // The heap is a large private commit; skip mapped images and reserves.
        const bool readable = info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
                              (info.Protect & PAGE_READWRITE) != 0 &&
                              (info.Protect & PAGE_GUARD) == 0;
        if (readable && regionSize >= PROLOGUE_SPAN) {
            regionsRead++;
            for (uint64_t offset = 0; offset < regionSize && heapBase == 0; offset += SCAN_CHUNK) {
                const size_t want = static_cast<size_t>(
                    std::min<uint64_t>(SCAN_CHUNK + PROLOGUE_SPAN, regionSize - offset));
                SIZE_T got = 0;
                if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(regionBase + offset),
                                       buffer.data(), want, &got) || got < PROLOGUE_SPAN)
                    break;
                scanned += got;

                for (size_t i = 0; i + PROLOGUE_SPAN <= got; i += Spur::BYTES_PER_OOP) {
                    const uint64_t* words = reinterpret_cast<const uint64_t*>(buffer.data() + i);
                    if (!isZeroSlotObjectOfClass(words[0], NIL_CLASS_INDEX)) continue;
                    if (!isZeroSlotObjectOfClass(words[2], NIL_CLASS_INDEX + 1)) continue;
                    if (!isZeroSlotObjectOfClass(words[4], NIL_CLASS_INDEX + 2)) continue;
                    heapBase = regionBase + offset + i;
                    break;
                }
            }
        }

        const uint64_t next = regionBase + regionSize;
        if (next <= address) break;
        address = next;
    }
    CloseHandle(process);

    const std::string surveyed =
        " (pid " + std::to_string(*pid) + ", " + std::to_string(regionsWalked)
        + " regions, " + std::to_string(regionsRead) + " of them read, "
        + std::to_string(scanned >> 20) + " MB scanned)";

    if (heapBase == 0) {
        why = "no nil/false/true prologue anywhere in the target" + surveyed;
        return false;
    }
    if (!prologueMatchesAt(heapBase)) {
        why = "the prologue found by scanning does not read back through the "
              "debugger" + surveyed;
        return false;
    }

    found.nilObj = heapBase;
    found.falseObj = heapBase + PROLOGUE_STRIDE;
    found.trueObj = heapBase + 2 * PROLOGUE_STRIDE;
    found.hiddenRootsObj = objectAfter(objectAfter(found.trueObj));
    if (!looksLikeClassTable(found.hiddenRootsObj)) {
        why = "no class table after the prologue found at 0x" + hexOf(heapBase);
        return false;
    }

    // Old space keeps its layout across a load, so the special objects array
    // sits at the same offset from the base as it did in the image file.
    if (imageBase == 0 || imageSpecialObjects == 0) {
        why = "the heap was found at 0x" + hexOf(heapBase)
            + " but no image file was named to place the special objects array";
        return false;
    }
    found.specialObjectsOop = imageSpecialObjects - imageBase + heapBase;
    const uint64_t header = readWord(found.specialObjectsOop);
    if (((header >> Spur::FORMAT_SHIFT) & Spur::FORMAT_MASK) != Fmt::VARIABLE) {
        why = "the special objects array is not where the image file puts it, "
              "relative to the heap at 0x" + hexOf(heapBase);
        return false;
    }
    return true;
}

// An allocation is reported as a run of regions, each of one protection, that
// share its base.
std::pair<uint64_t, uint64_t> SpurProcessMemory::allocationContaining(uint64_t address) const {
    auto pid = reader->attachedPid();
    if (!pid || address == 0) return {0, 0};
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(*pid));
    if (process == nullptr) return {0, 0};
    MEMORY_BASIC_INFORMATION info;
    uint64_t start = 0;
    uint64_t end = 0;
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != 0 &&
        info.State != MEM_FREE) {
        start = reinterpret_cast<uint64_t>(info.AllocationBase);
        end = start;
        while (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(end), &info, sizeof(info)) != 0 &&
               reinterpret_cast<uint64_t>(info.AllocationBase) == start)
            end = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
    }
    CloseHandle(process);
    return {start, end};
}

#else

std::pair<uint64_t, uint64_t> SpurProcessMemory::allocationContaining(uint64_t) const {
    return {0, 0};
}

bool SpurProcessMemory::rootsFromPrologueScan(SpurRoots&, uint64_t, uint64_t,
                                              std::string& why) const {
    why = "scanning a 64-bit target needs an x64 build of this server";
    return false;
}

#endif

bool SpurProcessMemory::locateRoots(uint64_t imageBase, uint64_t imageEnd,
                                    uint64_t imageSpecialObjects) {
    base = imageBase;
    end = imageEnd;

    SpurRoots found;
    bool located = rootsFromSymbols(found);
    if (!located) {
        found = SpurRoots{};
        std::string why;
        located = rootsFromPrologueScan(found, imageBase, imageSpecialObjects, why);
        if (!located) {
            lastError = "could not locate the Spur heap in the target process: "
                        "no interpreter symbols, and " + why;
            return false;
        }
    }

    if (!looksLikeClassTable(found.hiddenRootsObj)) {
        lastError = "class table does not look like a Spur hidden roots object";
        return false;
    }

    // A relocated heap keeps its own base, so bound walks by where nil landed.
    if (base == 0 || found.nilObj < base || found.nilObj >= end) {
        const uint64_t span = end > base ? end - base : 0;
        base = found.nilObj;
        end = span != 0 ? base + span : UINT64_MAX;
    }

    found.valid = true;
    vmRoots = found;
    return true;
}

} // namespace osvm
