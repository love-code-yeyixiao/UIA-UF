#pragma once

#include <windows.h>

#include <string>

namespace uiauf {

// Records the client binary (path, SHA-256, last write time) the first time the
// service starts, plus the installation mode.
//
// `setupMode` is only non-null when the service control manager started us with
// an explicit --standalone / --bundled argument (i.e. straight from --install);
// it is persisted so later SCM starts keep the same policy.
DWORD EnsureSetupRegistration(const wchar_t* setupMode, std::wstring& message);

// Evaluates the idle/unused policy for a user session. `sessionId` may be 0
// when no interactive session is available: the warning then cannot be shown
// (and the grace clock is not started), but an already warned installation can
// still reach the auto-disable step. `stopEvent` is signalled after the service
// disables itself so ServiceMain can shut down cleanly.
void EvaluateIdlePolicy(DWORD sessionId, HANDLE stopEvent);

} // namespace uiauf
