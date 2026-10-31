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

// Evaluates the idle/unused policy for a user session and, when it applies,
// asks the registered client to show a warning notification in that session.
// Safe to call repeatedly; it only acts once per warning period.
void EvaluateIdlePolicy(DWORD sessionId);

} // namespace uiauf
