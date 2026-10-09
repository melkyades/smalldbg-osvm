#pragma once
#include "Smalltalk/OSVM/OSVMInspector.h"
#include <cstdint>
#include <string>
namespace webside {
/// The image's memory regions.
std::string osvmRegionsJson(const osvm::OSVMInspector& inspector);
/// What lives at an address.
std::string osvmClassifyJson(const osvm::OSVMInspector& inspector, uint64_t address);
} // namespace webside
