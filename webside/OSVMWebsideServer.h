#pragma once

#include "Smalltalk/WebsideServer.h"
#include "OSVMDebugSession.h"
#include "Workspace.h"
#include <string>

namespace osvm {

/** An OSVMWebsideServer is the Webside server for OpenSmalltalk images: Cuis, and
 *  Squeak-family images that share the Spur object format. **/
class OSVMWebsideServer : public smalltalk::WebsideServer {
public:
    OSVMWebsideServer(webside::Workspace& workspace, int port)
        : smalltalk::WebsideServer(workspace, port) {}

protected:
    std::string dialect() const override { return "Cuis"; }
    std::string description() const override {
        return "OpenSmalltalk (Spur) image server on smalldbg";
    }
    void setupRoutes() override;

    webside::HttpResponse handleRegions(const webside::HttpRequest& req) const override;

private:
    OSVMDebugSession* osvmSession() const;

    /** Sends /debuggers/1/frames to the general dispatcher: an image has no
     *  native threads, so its Smalltalk processes start at debugger 1. **/
    void overrideDebuggerRoutes();
};

} // namespace osvm
