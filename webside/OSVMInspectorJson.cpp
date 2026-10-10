#include "OSVMInspectorJson.h"
#include "Json.h"
#include "Smalltalk/OSVM/OSVMInspector.h"

namespace webside {

using vmdbg::Json;

std::string osvmRootsJson(const osvm::OSVMInspector& inspector) {
    const osvm::SpurRoots& roots = inspector.imageHandle()->rootObjects();
    return Json::object()
        .set("nil", Json::hex(roots.nilObj))
        .set("false", Json::hex(roots.falseObj))
        .set("true", Json::hex(roots.trueObj))
        .set("classTable", Json::hex(roots.hiddenRootsObj))
        .set("specialObjects", Json::hex(roots.specialObjectsOop))
        .dump();
}

} // namespace webside
