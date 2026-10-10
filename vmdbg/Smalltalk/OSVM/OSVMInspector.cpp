#include "OSVMInspector.h"

namespace osvm {

bool OSVMInspector::refreshRegions() {
    if (!image) return false;
    clearRegions();
    vmdbg::Region oldSpace;
    oldSpace.name = "oldSpace";
    oldSpace.start = image->objectMemoryBase();
    oldSpace.end = image->objectMemoryEnd();
    oldSpace.kind = vmdbg::Region::Kind::Objects;
    addRegion(std::move(oldSpace));
    return true;
}

bool OSVMInspector::discoverClasses() {
    clearClasses();
    for (const auto& [name, cls] : image->classes()) {
        smalltalk::ClassEntry entry;
        entry.name = name;
        entry.oop = cls.oop;
        entry.superclassName = cls.superclassName;
        if (const auto* parent = image->findClass(cls.superclassName))
            entry.superclass = parent->oop;
        entry.category = cls.category;
        entry.package = cls.category;
        entry.instanceVariableNames = image->instanceVariableNamesOf(cls.oop);
        for (const auto& [variable, value] : image->classVariablesOf(cls.oop))
            entry.classVariableNames.push_back(variable);
        addClass(std::move(entry));
    }
    return !classes().empty();
}

std::vector<smalltalk::MethodEntry> OSVMInspector::methodsOf(
    const std::string& className) const {
    std::vector<smalltalk::MethodEntry> methods;
    for (const auto& method : image->methodsOf(image->resolveClassOop(className)))
        methods.push_back({method.selector, className, method.category,
                           method.oop});
    return methods;
}

std::string OSVMInspector::sourceOf(const smalltalk::MethodEntry& method) const {
    return image->sourceOfMethod(method.oop);
}

std::vector<smalltalk::ClassVariable> OSVMInspector::classVariablesOf(
    const std::string& className) const {
    std::vector<smalltalk::ClassVariable> variables;
    for (const auto& [name, value] :
         image->classVariablesOf(image->resolveClassOop(className)))
        variables.push_back({name, value});
    return variables;
}

} // namespace osvm
