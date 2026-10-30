#pragma once

#include <windows.h>

#include <cstddef>
#include <thread>
#include <vector>

#include "common/protocol.h"
#include "common/win32_helpers.h"

namespace uiauf {

// Owns the named pipe endpoint and the single worker thread that serves it.
// Requests are handled one at a time: this keeps the code small and removes a
// whole class of concurrency bugs at the cost of a tiny bit of latency.
class ServiceServer {
public:
    ServiceServer() = default;
    ~ServiceServer();

    ServiceServer(const ServiceServer&) = delete;
    ServiceServer& operator=(const ServiceServer&) = delete;

    bool Start();
    void RequestStop();
    void Wait();

    HANDLE StopEvent() const { return stopEvent_.get(); }

private:
    void Run();
    void HandleClient(HANDLE pipe);
    void HandleLaunch(HANDLE pipe, const std::vector<std::byte>& payload, LaunchResult& result);

    UniqueHandle stopEvent_;
    std::thread thread_;
};

} // namespace uiauf
