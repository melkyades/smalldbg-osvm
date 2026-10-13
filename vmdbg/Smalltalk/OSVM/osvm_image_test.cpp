// Exercises the Spur object model against a real image file, with no VM and
// no debugger involved. Run: osvm_image_test <path/to/Cuis.image>
#include "SpurImage.h"
#include "SpurImageFile.h"
#include "SpurKnownObjects.h"
#include "SpurMethodMap.h"
#include <set>
#include <limits>
#include <vector>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::cout << "  ok    " << what << std::endl;
        return;
    }
    std::cout << "  FAIL  " << what << std::endl;
    failures++;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: osvm_image_test <image>" << std::endl;
        return 2;
    }

    osvm::SpurImageFile file;
    if (!file.open(argv[1])) {
        std::cerr << "open failed: " << file.error() << std::endl;
        return 1;
    }
    std::cout << "image version " << file.version()
              << " base 0x" << std::hex << file.baseAddress()
              << " special 0x" << file.specialObjectsOop() << std::dec << std::endl;

    osvm::SpurImage image(&file);
    image.sourceFiles().locateFor(argv[1]);
    if (!image.load()) {
        std::cerr << "load failed: " << image.error() << std::endl;
        return 1;
    }

    std::cout << "classes: " << image.classes().size() << std::endl;
    check(image.classes().size() > 500, "discovered a plausible number of classes");
    check(image.findClass("Object") != nullptr, "Object is present");
    check(image.findClass("Array") != nullptr, "Array is present");

    const auto* object = image.findClass("Object");
    check(object->superclassName == "ProtoObject", "Object superclass is ProtoObject");

    const uint64_t arrayOop = image.resolveClassOop("Array");
    check(image.nameOfClass(arrayOop) == "Array", "Array resolves by name");
    check(image.nameOfClass(image.resolveClassOop("Array class")) == "Array class",
          "Array class resolves as a metaclass");

    const auto methods = image.methodsOf(arrayOop);
    std::cout << "Array methods: " << methods.size() << std::endl;
    check(!methods.empty(), "Array has methods");

    const uint64_t printOn = image.methodNamed(image.resolveClassOop("Object"), "printOn:");
    check(printOn != 0, "Object>>printOn: found");
    const std::string source = image.sourceOfMethod(printOn);
    check(source.rfind("printOn:", 0) == 0, "printOn: source starts with its selector");

    const auto header = image.headerOfMethod(printOn);
    check(header.numArgs == 1, "printOn: takes one argument");

    // ---- SpurObject / SpurHeapObject ----
    const osvm::SpurHeapObject arrayClass = image.heapObjectAt(arrayOop);
    check(arrayClass.isClass(), "Array class is a class");
    check(!arrayClass.isMetaclass(), "Array class is not a metaclass");
    check(image.heapObjectAt(image.resolveClassOop("Array class")).isMetaclass(),
          "Array class's metaclass is a metaclass");
    check(arrayClass.rawNameSlot() == "Array", "name slot reads through the heap object");
    check(arrayClass.nameIfClass() == "Array", "nameIfClass on a class");
    check(arrayClass.className() == "Array class", "an Array class's class is its metaclass");
    check(arrayClass.isPointers() && !arrayClass.isBytes(), "a class is a pointer object");
    check(arrayClass.heapSlotAt(0).nameIfClass() == "ArrayedCollection",
          "superclass slot reads as a class");

    const osvm::SpurHeapObject nameSymbol = arrayClass.heapSlotAt(6);
    check(nameSymbol.isBytes(), "a Symbol is a byte object");
    check(nameSymbol.stringValue() == "Array", "Symbol stringValue");
    check(nameSymbol.printString() == "#Array", "Symbol printString");
    check(nameSymbol.byteSize() == 5, "Symbol byteSize excludes the odd-byte padding");

    const osvm::SpurSmallInteger smallInt(osvm::SpurSmallInteger::encode(5), &image);
    check(smallInt.isImmediate() && smallInt.isSmallInteger(), "SmallInteger is immediate");
    check(smallInt.value() == 5, "SmallInteger decodes with 3 tag bits");
    check(smallInt.printString() == "5", "SmallInteger printString");
    check(smallInt.className() == "SmallInteger", "SmallInteger resolves its class");

    // encode / value round-trip across the 61-bit range, including the edges
    // that a 63-bit (wrong-shift) decode would get wrong.
    bool roundTrips = true;
    for (int64_t v : {int64_t{0}, int64_t{1}, int64_t{-1}, int64_t{42}, int64_t{-42},
                      osvm::SpurSmallInteger::MAX_VALUE, osvm::SpurSmallInteger::MIN_VALUE})
        roundTrips = roundTrips &&
            osvm::SpurSmallInteger(osvm::SpurSmallInteger::encode(v), &image).value() == v;
    check(roundTrips, "SmallInteger encode/value round-trips over the whole range");
    check(osvm::SpurSmallInteger::MAX_VALUE == 1152921504606846975,
          "SmallInteger maxVal matches the image's 61-bit range");
    check(osvm::SpurSmallInteger::fits(osvm::SpurSmallInteger::MAX_VALUE) &&
          !osvm::SpurSmallInteger::fits(osvm::SpurSmallInteger::MAX_VALUE + 1),
          "SmallInteger fits() bounds the range");
    check(image.objectAt(image.nilObject()).asSmallInteger().valueOr(-7) == -7,
          "valueOr falls back when the oop is not a SmallInteger");
    check(image.classAt(image.resolveClassOop("Point")).heapObject()
              .slotAt(osvm::SpurBehavior::Slot::FORMAT).asSmallInteger().valueOr(0) != 0,
          "valueOr returns the real value for a genuine SmallInteger");

    // ---- Character ----
    const osvm::SpurCharacter letter(osvm::SpurCharacter::encode('A'), &image);
    check(letter.isCharacter() && !letter.isSmallInteger(), "Character carries its own tag");
    check(letter.value() == 'A', "Character decodes with 3 tag bits");
    check(letter.className() == "Character", "Character resolves its class");
    check(letter.printString() == "$A", "Character printString");
    check(letter.asUtf8() == "A", "Character asUtf8 for ASCII");
    const osvm::SpurCharacter lambda(osvm::SpurCharacter::encode(0x3BB), &image);
    check(lambda.asUtf8() == "\xCE\xBB", "Character asUtf8 for a 2-byte code point");
    check(osvm::SpurCharacter(osvm::SpurCharacter::encode(0x1F600), &image).asUtf8().size() == 4,
          "Character asUtf8 for a 4-byte code point");
    check(!osvm::SpurCharacter::isValidCodePoint(0xD800), "surrogates are not valid code points");
    check(image.objectAt(image.nilObject()).asCharacter().valueOr(7) == 7,
          "Character valueOr falls back");

    // ---- SmallFloat64 ----
    const osvm::SpurSmallFloat half(osvm::SpurSmallFloat::encode(0.5), &image);
    check(half.isSmallFloat(), "SmallFloat carries its own tag");
    check(half.value() == 0.5, "SmallFloat decodes");
    check(half.className() == "SmallFloat64", "SmallFloat resolves its class");
    check(half.printString() == "0.5", "SmallFloat printString");
    check(osvm::SpurSmallFloat(osvm::SpurSmallFloat::encode(30.0), &image).printString() == "30.0",
          "SmallFloat printString keeps a trailing .0");
    check(osvm::SpurSmallFloat(osvm::SpurSmallFloat::encode(7.2), &image).printString() == "7.2",
          "SmallFloat printString round-trips a repeating fraction");

    bool floatsRoundTrip = true;
    for (double v : {0.0, -0.0, 0.5, -0.5, 1.0, 7.2, 30.0, 1e30, 1e-30, 3.141592653589793})
        floatsRoundTrip = floatsRoundTrip &&
            osvm::SpurSmallFloat(osvm::SpurSmallFloat::encode(v), &image).value() == v;
    check(floatsRoundTrip, "SmallFloat encode/value round-trips");
    check(osvm::SpurSmallFloat::fits(1.0) && !osvm::SpurSmallFloat::fits(1e300),
          "SmallFloat fits() rejects an out-of-range exponent");
    check(!osvm::SpurSmallFloat::fits(std::numeric_limits<double>::infinity()),
          "SmallFloat fits() rejects infinity");
    check(image.objectAt(image.nilObject()).asSmallFloat().valueOr(-1.0) == -1.0,
          "SmallFloat valueOr falls back");

    // Every SmallFloat literal in the image must survive a decode/encode cycle.
    size_t floatLiterals = 0, floatFailures = 0;
    for (const auto& [name, entry] : image.classes()) {
        for (uint64_t side : {entry.oop, entry.metaclass}) {
            for (const auto& method : image.methodsOf(side)) {
                for (const auto& literal : image.methodAt(method.oop).literals()) {
                    if (!literal.isSmallFloat()) continue;
                    floatLiterals++;
                    const double decoded = literal.asSmallFloat().value();
                    if (osvm::SpurSmallFloat::encode(decoded) != literal.oop()) floatFailures++;
                }
            }
        }
    }
    std::cout << "SmallFloat literals: " << floatLiterals
              << " failed re-encode: " << floatFailures << std::endl;
    check(floatLiterals > 500, "the image holds a useful number of SmallFloat literals");
    check(floatFailures == 0, "every SmallFloat literal re-encodes to its own oop");

    check(image.objectAt(image.nilObject()).isNil(), "nil is nil");
    check(image.objectAt(image.falseObject()).printString() == "false", "false prints");
    check(image.objectAt(image.trueObject()).printString() == "true", "true prints");
    check(!image.objectAt(image.nilObject()).isPresent(), "nil is not present");

    const osvm::SpurHeapObject specials = image.heapObjectAt(file.specialObjectsOop());
    check(specials.className() == "Array", "special objects array is an Array");
    check(specials.nextObject().oop() > specials.oop(), "nextObject moves forward");

    // ---- known objects ----
    const osvm::SpurClass point = image.classAt(image.resolveClassOop("Point"));
    check(point.name() == "Point", "SpurClass name");
    check(point.superclass().heapObject().nameIfClass() == "Object", "SpurBehavior superclass");
    check(point.instanceVariableNames() == std::vector<std::string>({"x", "y"}),
          "SpurClassDescription instanceVariableNames");
    check(point.instSpec() == 1 && point.fixedFields() == 2,
          "SpurBehavior decodes instSpec and fixedFields");
    check(point.organization().categoryOfElement("dist:") == "point functions",
          "SpurClassOrganizer categoryOfElement");
    check(point.organization().classComment().find("x-y pair") != std::string::npos,
          "SpurClassOrganizer classComment via RemoteString");

    const osvm::SpurMethodDictionary points = point.methodDictionary();
    check(points.tally() == static_cast<int64_t>(points.entries().size()),
          "SpurMethodDictionary tally matches its occupied entries");

    osvm::SpurCompiledMethod dist;
    for (const auto& [selector, method] : points.entries())
        if (selector == "dist:") dist = method;
    check(bool(dist), "SpurMethodDictionary finds dist:");
    check(dist.header().numArgs == 1, "SpurCompiledMethod numArgs");
    check(dist.literals().size() == dist.header().numLiterals, "SpurCompiledMethod literals");
    check(dist.sourcePointer() != 0, "SpurCompiledMethod has a source pointer");
    check(dist.sourceCode().rfind("dist:", 0) == 0, "SpurCompiledMethod sourceCode");

    const osvm::SpurMetaclass pointMeta(image.heapObjectAt(image.resolveClassOop("Point class")));
    check(pointMeta.isMetaclass(), "SpurMetaclass isMetaclass");
    check(pointMeta.thisClass().name() == "Point", "SpurMetaclass thisClass");
    check(pointMeta.name() == "Point class", "SpurMetaclass name");

    const osvm::SpurSystemDictionary globals = image.systemDictionary();
    check(globals.tally() == static_cast<int64_t>(globals.associations().size()),
          "SpurSystemDictionary tally matches its associations");

    const osvm::SpurProcessorScheduler processor = image.scheduler();
    check(bool(processor), "Processor found through the special objects array");
    const osvm::SpurProcess active = processor.activeProcess();
    check(bool(active), "scheduler has an active process");
    check(active.priority() > 0, "SpurProcess priority");
    const osvm::SpurContext top = active.suspendedContext();
    check(bool(top), "active process has a suspended context");
    check(bool(top.method()), "SpurContext method");
    check(top.receiver().className() == "SystemDictionary",
          "a saved image is suspended inside SystemDictionary");
    check(top.temporaries().size() >= top.method().header().numArgs,
          "SpurContext temporaries cover the arguments");
    check(bool(top.sender()), "SpurContext sender");

    check(!image.categoryOfClass("Object").empty(), "Object has a system category");
    check(!image.allInstanceVariableNamesOf(image.resolveClassOop("Point")).empty(),
          "Point has instance variables");

    // Every method in the image must resolve to source that begins with its
    // own selector; this is the property the source-pointer decode has to hold.
    size_t checked = 0, mismatched = 0, missing = 0;
    for (const auto& [name, entry] : image.classes()) {
        for (uint64_t side : {entry.oop, entry.metaclass}) {
            for (const auto& method : image.methodsOf(side)) {
                const std::string text = image.sourceOfMethod(method.oop);
                if (text.empty()) { missing++; continue; }
                const std::string first = method.selector.substr(0, method.selector.find(':'));
                size_t start = text.find_first_not_of(" \t\r\n");
                if (start == std::string::npos || text.compare(start, first.size(), first) != 0)
                    mismatched++;
                checked++;
            }
        }
    }
    std::cout << "methods checked: " << checked
              << " mismatched: " << mismatched
              << " missing source: " << missing << std::endl;
    check(mismatched == 0, "every method's source begins with its selector");
    check(missing == 0, "every method has source");

    // Every method decodes into bytecodes that cover its bytes exactly, with
    // every jump landing on a bytecode. The source places the sends and blocks
    // of nearly every method; the rest are reported.
    size_t decoded = 0, badBytes = 0, badJumps = 0, unassigned = 0;
    size_t sendsPlaced = 0, withSends = 0, blocksNamed = 0, withBlocks = 0;
    for (const auto& [name, entry] : image.classes()) {
        for (uint64_t side : {entry.oop, entry.metaclass}) {
            for (const auto& method : image.methodsOf(side)) {
                const osvm::SpurCompiledMethod compiled = image.methodAt(method.oop);
                const osvm::SpurMethodMap map(compiled);
                const auto& bytecodes = map.bytecodes();
                decoded++;
                size_t bytes = 0;
                std::set<int> starts;
                for (const auto& bytecode : bytecodes) {
                    bytes += bytecode.bytes.size();
                    starts.insert(bytecode.pc);
                    if (bytecode.text == "unassigned") unassigned++;
                }
                if (bytes != compiled.bytecodes().size()) badBytes++;
                const int end = compiled.initialPC() + static_cast<int>(bytes);
                for (const auto& bytecode : bytecodes)
                    if (bytecode.target && *bytecode.target != end && !starts.count(*bytecode.target))
                        badJumps++;
                for (size_t i = 0; i < bytecodes.size(); i++) {
                    if (bytecodes[i].selector.empty()) continue;
                    withSends++;
                    sendsPlaced += map.sourceOf(i).has_value();
                    break;
                }
                if (!osvm::decodeMethod(compiled).closures.empty()) {
                    withBlocks++;
                    blocksNamed += map.namesBlocks();
                }
            }
        }
    }
    std::cout << "methods decoded: " << decoded << " bad byte counts: " << badBytes
              << " bad jumps: " << badJumps << " unassigned: " << unassigned << std::endl;
    std::cout << "sends placed in source: " << sendsPlaced << "/" << withSends
              << ", blocks named: " << blocksNamed << "/" << withBlocks << std::endl;
    check(badBytes == 0, "every method's bytecodes cover its bytes");
    check(badJumps == 0, "every jump lands on a bytecode");
    check(unassigned == 0, "no method uses an unassigned bytecode");
    check(sendsPlaced * 100 >= withSends * 95, "the source places the sends of most methods");
    check(blocksNamed * 100 >= withBlocks * 97, "the source names the blocks of most methods");

    std::cout << "\n--- Object>>printOn: ---\n" << source << std::endl;
    std::cout << "\n--- Object definition ---\n"
              << image.definitionOfClass(image.resolveClassOop("Object")) << std::endl;

    std::cout << (failures == 0 ? "\nALL CHECKS PASSED" : "\nFAILURES") << std::endl;
    return failures == 0 ? 0 : 1;
}
