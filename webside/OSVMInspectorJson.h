#pragma once
#include "Smalltalk/OSVM/OSVMInspector.h"
#include <cstdint>
#include <string>
namespace webside {
/** Returns the addresses of the image's roots (nil, true, false, class table, special
 *  objects) as a JSON object. **/
std::string osvmRootsJson(const osvm::OSVMInspector& inspector);
} // namespace webside
