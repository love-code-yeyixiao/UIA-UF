#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace uiauf {

// Minimal append-only log under %ProgramData%\UIA-UF\service.log.
void LogLine(std::wstring_view text);
void LogError(std::wstring_view what, DWORD error);

} // namespace uiauf
