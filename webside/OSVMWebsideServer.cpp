#include "OSVMWebsideServer.h"
#include "OSVMInspectorJson.h"
#include "Json.h"
#include <iostream>

using webside::HttpRequest;
using webside::HttpResponse;
using webside::Json;

namespace osvm {

OSVMDebugSession* OSVMWebsideServer::osvmSession() const {
    return workspace.sessionAs<OSVMDebugSession>();
}

void OSVMWebsideServer::setupRoutes() {
    WebsideServer::setupRoutes();
    overrideDebuggerRoutes();
}

void OSVMWebsideServer::overrideDebuggerRoutes() {
    // The base reserves debugger 1 for the native stack. An image has no
    // native threads, so its Smalltalk processes start at 1 instead and these
    // two routes have to go through the general dispatcher to reach them.
    const auto dispatch = [this](const HttpRequest& req) {
        return handleDebuggerRoute(req);
    };
    server.route("GET", "/debuggers/1/frames", dispatch);
    server.routePrefix("GET", "/debuggers/1/frames", dispatch);
}

// An image's roots are where a reader starts, and the class count says how
// much of it was understood.
HttpResponse OSVMWebsideServer::handleRegions(const HttpRequest& req) const {
    HttpResponse res = WebsideServer::handleRegions(req);
    auto* osvm = osvmSession();
    if (osvm == nullptr || res.body == "{}") return res;
    auto json = Json::parse(res.body);
    json.set("roots", Json::parse(webside::osvmRootsJson(osvm->inspector())));
    json.set("classes", static_cast<int64_t>(
        osvm->inspector().imageHandle()->classes().size()));
    res.body = json.dump();
    return res;
}

} // namespace osvm
