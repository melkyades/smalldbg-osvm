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

void OSVMWebsideServer::onPostSuspend() {
    auto* osvm = osvmSession();
    if (osvm != nullptr) osvm->refreshGreenThreads();
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

    // An image-only session has no process behind it, so answer these
    // directly rather than letting the base handlers reach for a debugger.
    server.route("GET", "/memory", [this](const HttpRequest& req) {
        HttpResponse res;
        auto* osvm = osvmSession();
        auto address = req.params.find("address");
        if (osvm == nullptr || address == req.params.end()) {
            res.statusCode = 400;
            res.body = Json::object().set("error", "address is required").dump();
            return res;
        }
        size_t count = 64;
        auto requested = req.params.find("count");
        if (requested != req.params.end()) count = std::stoul(requested->second);
        res.body = osvm->readMemory(parseHexParam(address->second), count);
        return res;
    });

    server.route("GET", "/disassemble", [](const HttpRequest&) {
        HttpResponse res;
        res.statusCode = 501;
        res.body = Json::object()
            .set("error", "disassembly is not available for Smalltalk bytecode")
            .dump();
        return res;
    });

    server.route("GET", "/symbol", [](const HttpRequest&) {
        HttpResponse res;
        res.body = Json::object().set("error", "no native symbols").dump();
        res.statusCode = 404;
        return res;
    });
}

HttpResponse OSVMWebsideServer::handleRegions(const HttpRequest&) const {
    HttpResponse res;
    auto* osvm = osvmSession();
    res.body = osvm != nullptr
        ? webside::osvmRegionsJson(osvm->inspector())
        : "{}";
    return res;
}

HttpResponse OSVMWebsideServer::handleClassify(const HttpRequest& req) const {
    HttpResponse res;
    auto* osvm = osvmSession();
    auto address = req.params.find("address");
    if (osvm == nullptr || address == req.params.end()) {
        res.statusCode = 400;
        res.body = Json::object().set("error", "address is required").dump();
        return res;
    }
    res.body = webside::osvmClassifyJson(osvm->inspector(), parseHexParam(address->second));
    return res;
}

HttpResponse OSVMWebsideServer::handleInspect(const HttpRequest& req) const {
    HttpResponse res;
    auto* osvm = osvmSession();
    auto oop = req.params.find("oop");
    if (osvm == nullptr || oop == req.params.end()) {
        res.statusCode = 400;
        res.body = Json::object().set("error", "oop is required").dump();
        return res;
    }
    int maxSlots = 64;
    auto requested = req.params.find("maxSlots");
    if (requested != req.params.end()) maxSlots = std::stoi(requested->second);
    res.body = webside::inspectOSVMObject(osvm->inspector(), parseHexParam(oop->second), maxSlots);
    return res;
}

std::string OSVMWebsideServer::nativeSymbolsData(const std::string& filter) const {
    if (!isActive()) return "[]";
    return WebsideServer::nativeSymbolsData(filter);
}

std::string OSVMWebsideServer::nativeModulesData() const {
    if (!isActive()) return "[]";
    return WebsideServer::nativeModulesData();
}

std::string OSVMWebsideServer::nativeSymbolDetailData(const std::string& name) const {
    if (!isActive()) return "{}";
    return WebsideServer::nativeSymbolDetailData(name);
}

std::string OSVMWebsideServer::nativeInspectData(const std::string& expression) const {
    if (!isActive()) return "{}";
    return WebsideServer::nativeInspectData(expression);
}

} // namespace osvm
