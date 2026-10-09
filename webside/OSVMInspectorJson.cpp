#include "OSVMInspectorJson.h"
#include "Json.h"
#include "Smalltalk/OSVM/OSVMInspector.h"

namespace webside {

using vmdbg::Json;

std::string osvmClassifyJson(const osvm::OSVMInspector& inspector, uint64_t address) {
    const osvm::SpurObject object = inspector.imageHandle()->objectAt(address);
    auto json = Json::object().set("address", Json::hex(address));

    if (object.isImmediate()) {
        return json.set("kind", "immediate")
                   .set("class", object.className())
                   .set("value", object.printString())
                   .dump();
    }
    if (!inspector.imageHandle()->memoryReader()->contains(address)) {
        return json.set("kind", "outside").dump();
    }
    return json.set("kind", "heap")
               .set("class", object.className())
               .set("value", object.printString())
               .dump();
}

std::string osvmRegionsJson(const osvm::OSVMInspector& inspector) {
    const osvm::SpurRoots& roots = inspector.imageHandle()->rootObjects();
    auto heap = Json::object()
        .set("name", "old space")
        .set("start", Json::hex(inspector.imageHandle()->memoryReader()->baseAddress()))
        .set("end", Json::hex(inspector.imageHandle()->memoryReader()->endAddress()))
        .set("live", inspector.imageHandle()->memoryReader()->isLive());

    auto rootsJson = Json::object()
        .set("nil", Json::hex(roots.nilObj))
        .set("false", Json::hex(roots.falseObj))
        .set("true", Json::hex(roots.trueObj))
        .set("classTable", Json::hex(roots.hiddenRootsObj))
        .set("specialObjects", Json::hex(roots.specialObjectsOop));

    return Json::object()
        .set("regions", Json::array().add(heap))
        .set("roots", rootsJson)
        .set("classes", static_cast<int64_t>(inspector.imageHandle()->classes().size()))
        .dump();
}

} // namespace webside
