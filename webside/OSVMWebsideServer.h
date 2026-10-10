#pragma once

#include "Smalltalk/WebsideServer.h"
#include "OSVMDebugSession.h"
#include "Workspace.h"
#include <string>

namespace osvm {

/// Webside server for OpenSmalltalk images (Cuis, and Squeak-family images
/// sharing the Spur object format).
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
    void onPostSuspend() override;

    webside::HttpResponse handleRegions(const webside::HttpRequest& req) const override;

    std::string nativeSymbolsData(const std::string& filter) const override;
    std::string nativeModulesData() const override;
    std::string nativeSymbolDetailData(const std::string& name) const override;
    std::string nativeInspectData(const std::string& expression) const override;

private:
    OSVMDebugSession* osvmSession() const;

    /// Replace the base routes that assume a live debugger, so an image-only
    /// session answers instead of dereferencing one.
    void overrideDebuggerRoutes();
};

} // namespace osvm
