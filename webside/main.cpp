#include "OSVMWebsideServer.h"
#include "OSVMDebugSession.h"
#include "Main.h"

int main(int argc, char** argv) {
    webside::DialectInfo osvm;
    osvm.name = "osvm";
    osvm.targetSyntax =
        "<image.image | vm_executable | dump.dmp | trace.run> | --pid N";
    // An image is opened as a launch: the session recognises the name and
    // reads the file instead of running anything, so no VM is needed to browse
    // one. A dump, a trace or an attach may still name the image after the
    // double dash, where it is only a hint for where the heap is.
    osvm.extraUsage =
        "  image.image       browse a Spur image file directly, with no VM\n"
        "  vm_executable     launch an OpenSmalltalk VM; name the image after --\n";
    return webside::runWebside<osvm::OSVMWebsideServer>(
        argc, argv,
        [](const vmdbg::TargetSpec& target) {
            return std::make_unique<osvm::OSVMDebugSession>(vmdbg::debuggeeFor(target.kind));
        }, osvm);
}
