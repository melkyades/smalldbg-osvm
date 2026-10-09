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

std::string inspectOSVMObject(const osvm::OSVMInspector& inspector, uint64_t oop, int maxSlots) {
    if (oop == 0) return Json::object().set("error", "no oop given").dump();

    const osvm::SpurObject object = inspector.imageHandle()->objectAt(oop);
    auto json = Json::object()
        .set("oop", Json::hex(oop))
        .set("class", object.className())
        .set("printString", object.printString());

    if (object.isImmediate()) {
        json.set("immediate", true)
            .set("size", 0)
            .set("slots", Json::array());
        return json.dump();
    }

    const osvm::SpurHeapObject heapObject = object.asHeapObject();
    const uint64_t slots = heapObject.numSlots();
    json.set("immediate", false)
        .set("format", static_cast<int>(heapObject.format()))
        .set("classIndex", static_cast<int>(heapObject.classIndex()))
        .set("hash", static_cast<int>(heapObject.identityHash()))
        .set("size", static_cast<int64_t>(slots));

    if (heapObject.isBytes()) {
        json.set("bytes", heapObject.bytesAsString());
        json.set("slots", Json::array());
        return json.dump();
    }

    auto array = Json::array();
    const uint64_t limit = maxSlots <= 0 ? slots
                                         : std::min<uint64_t>(slots, static_cast<uint64_t>(maxSlots));
    for (uint64_t i = 0; i < limit; i++) {
        const osvm::SpurObject value = heapObject.slotAt(i);
        array.add(Json::object()
            .set("index", static_cast<int64_t>(i))
            .set("raw", Json::hex(value.oop()))
            .set("class", value.className())
            .set("value", value.printString()));
    }
    json.set("slots", array);
    return json.dump();
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
