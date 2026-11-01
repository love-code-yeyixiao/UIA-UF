// UIA-UF.Manager：面向用户输入的菜单式管理器。
//
// 它本身不实现任何业务逻辑，所有操作都通过调用同目录下的 uiauf.exe 完成，
// 这样"白名单只能经服务修改""哈希由调用方提供"这些约束仍然只有一处实现。
//
// 添加白名单时一定会把哈希完整打印出来并要求确认（默认 N），确认后才把该哈希
// 作为 --sha256 传给 uiauf。

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <commdlg.h>

#include <cstdio>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <string>
#include <vector>

#include "common/hash.h"
#include "common/protocol.h"
#include "common/win32_helpers.h"

namespace {

using uiauf::UniqueHandle;

const wchar_t kTitle[] = L"UIA-UF 管理器";

void EnableUnicodeConsole() {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
}

std::wstring Trim(std::wstring text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::wstring ReadLine() {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
        wchar_t buffer[1024]{};
        DWORD read = 0;
        if (ReadConsoleW(input, buffer, static_cast<DWORD>(std::size(buffer) - 1), &read, nullptr) && read > 0) {
            return Trim(std::wstring(buffer, read));
        }
        return {};
    }

    // 重定向场景（脚本/测试）只处理 ASCII。
    std::string line;
    std::getline(std::cin, line);
    return Trim(std::wstring(line.begin(), line.end()));
}

void Pause() {
    std::wprintf(L"\n  按回车返回菜单…");
    ReadLine();
}

bool Confirm(const wchar_t* prompt) {
    std::wprintf(L"%ls[y/N] ", prompt);
    const std::wstring answer = ReadLine();
    return _wcsicmp(answer.c_str(), L"y") == 0 || _wcsicmp(answer.c_str(), L"yes") == 0;
}

std::wstring CliPath() {
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (length == 0 || length >= std::size(module)) {
        return {};
    }
    return uiauf::ParentDirectory(std::wstring(module, length)) + L"\\uiauf.exe";
}

bool RunCaptured(const std::wstring& executable, const std::wstring& arguments, std::wstring& output,
                 DWORD& exitCode) {
    output.clear();
    exitCode = 1;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &attributes, 0)) {
        return false;
    }
    UniqueHandle reader(readEnd);
    UniqueHandle writer(writeEnd);
    SetHandleInformation(reader.get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writer.get();
    startup.hStdError = writer.get();

    std::wstring commandLine = uiauf::QuoteArgument(executable) + L" " + arguments;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child)) {
        return false;
    }
    UniqueHandle process(child.hProcess);
    UniqueHandle thread(child.hThread);

    // 关闭写端，否则 ReadFile 等不到 EOF。
    writer.reset();

    std::string bytes;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(reader.get(), buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        bytes.append(buffer, read);
    }

    WaitForSingleObject(process.get(), INFINITE);
    GetExitCodeProcess(process.get(), &exitCode);
    output = Utf8ToWide(bytes);
    return true;
}

enum class ServiceState { Unknown, NotInstalled, Stopped, Running, Disabled };

ServiceState QueryServiceState() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        return ServiceState::Unknown;
    }

    SC_HANDLE service = OpenServiceW(scm, uiauf::kServiceName, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        return error == ERROR_SERVICE_DOES_NOT_EXIST ? ServiceState::NotInstalled : ServiceState::Unknown;
    }

    SERVICE_STATUS status{};
    const BOOL statusOk = QueryServiceStatus(service, &status);

    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    std::vector<BYTE> buffer(needed > 0 ? needed : 0);
    auto* config = buffer.empty() ? nullptr : reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
    const BOOL configOk = config != nullptr && QueryServiceConfigW(service, config, needed, &needed);

    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    if (configOk && config->dwStartType == SERVICE_DISABLED) {
        return ServiceState::Disabled;
    }
    if (statusOk && status.dwCurrentState == SERVICE_RUNNING) {
        return ServiceState::Running;
    }
    return ServiceState::Stopped;
}

void PrintHeader() {
    const wchar_t* text = L"未知";
    switch (QueryServiceState()) {
    case ServiceState::Running:
        text = L"运行中";
        break;
    case ServiceState::Stopped:
        text = L"已停止";
        break;
    case ServiceState::Disabled:
        text = L"已禁用（以管理员运行 UIA-UF.Service.exe --install 可恢复）";
        break;
    case ServiceState::NotInstalled:
        text = L"未安装（以管理员运行 UIA-UF.Service.exe --install）";
        break;
    default:
        break;
    }

    std::wprintf(L"\n  %ls\n", kTitle);
    std::wprintf(L"  ----------------------------------------\n");
    std::wprintf(L"  服务：%ls\n", text);
    if (!uiauf::IsElevated()) {
        std::wprintf(L"  权限：未以管理员运行，添加/删除白名单会被服务拒绝\n");
    }
    std::wprintf(L"\n");
}

void PrintMenu() {
    std::wprintf(L"  1. 添加程序到白名单\n"
                 L"  2. 查看白名单\n"
                 L"  3. 删除白名单条目\n"
                 L"  4. 以 UIAccess 启动程序\n"
                 L"  5. 检查进程是否具有 UIAccess\n"
                 L"  6. 为现有快捷方式创建 UIAccess 副本\n"
                 L"  0. 退出\n"
                 L"\n  请选择：");
}

bool PickFile(std::wstring& path, const wchar_t* filter, const wchar_t* title) {
    wchar_t buffer[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetConsoleWindow();
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog)) {
        return false;
    }
    path = buffer;
    return true;
}

constexpr wchar_t kExeFilter[] = L"程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
constexpr wchar_t kLinkFilter[] = L"快捷方式 (*.lnk)\0*.lnk\0";

// 读取现有 .lnk 的目标/参数/起始位置/图标，生成一个指向 uiauf run 的副本。
bool CreateUiaShortcut(const std::wstring& sourceLink, const std::wstring& cli, std::wstring& created,
                       std::wstring& error) {
    created.clear();
    error.clear();

    IShellLinkW* source = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&source)))) {
        error = L"无法创建 ShellLink 对象。";
        return false;
    }

    IPersistFile* sourceFile = nullptr;
    bool loaded = false;
    if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&sourceFile)))) {
        loaded = SUCCEEDED(sourceFile->Load(sourceLink.c_str(), STGM_READ));
        sourceFile->Release();
    }

    wchar_t target[32768]{};
    wchar_t arguments[4096]{};
    wchar_t workingDirectory[32768]{};
    wchar_t iconPath[32768]{};
    wchar_t description[1024]{};
    int iconIndex = 0;
    int showCommand = SW_SHOWNORMAL;

    if (loaded) {
        source->GetPath(target, static_cast<int>(std::size(target)), nullptr, SLGP_RAWPATH);
        source->GetArguments(arguments, static_cast<int>(std::size(arguments)));
        source->GetWorkingDirectory(workingDirectory, static_cast<int>(std::size(workingDirectory)));
        source->GetIconLocation(iconPath, static_cast<int>(std::size(iconPath)), &iconIndex);
        source->GetDescription(description, static_cast<int>(std::size(description)));
        source->GetShowCmd(&showCommand);
    }
    source->Release();

    if (!loaded) {
        error = L"无法读取快捷方式：" + sourceLink;
        return false;
    }
    if (target[0] == L'\0') {
        error = L"这个快捷方式不直接指向文件（可能是 MSI 广告快捷方式），无法复制。";
        return false;
    }

    const std::wstring targetPath = target;
    const std::wstring working = workingDirectory;
    // 原快捷方式没显式设置图标时，Windows 用的是目标 exe 的图标；换成 uiauf 之后
    // 必须显式写回，否则图标会变成 UIA-UF 的。
    const std::wstring icon = iconPath[0] != L'\0' ? std::wstring(iconPath) : targetPath;

    const size_t dot = sourceLink.find_last_of(L'.');
    created = (dot == std::wstring::npos ? sourceLink : sourceLink.substr(0, dot)) + L" (UIAccess).lnk";

    std::wstring commandLine = L"run";
    if (!working.empty()) {
        commandLine += L" --cwd " + uiauf::QuoteArgument(working);
    }
    commandLine += L" " + uiauf::QuoteArgument(targetPath);
    if (arguments[0] != L'\0') {
        commandLine += L" ";
        commandLine += arguments;
    }

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        error = L"无法创建新的快捷方式对象。";
        return false;
    }

    link->SetPath(cli.c_str());
    link->SetArguments(commandLine.c_str());
    if (!working.empty()) {
        link->SetWorkingDirectory(working.c_str());
    }
    link->SetIconLocation(icon.c_str(), iconIndex);
    link->SetShowCmd(showCommand);

    const std::wstring fullDescription = std::wstring(description) + L"（UIAccess）";
    link->SetDescription(fullDescription.c_str());

    IPersistFile* linkFile = nullptr;
    bool saved = false;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&linkFile)))) {
        saved = SUCCEEDED(linkFile->Save(created.c_str(), TRUE));
        linkFile->Release();
    }
    link->Release();

    if (!saved) {
        error = L"写入快捷方式失败：" + created;
        return false;
    }
    return true;
}

void CreateShortcut(const std::wstring& cli) {
    std::wprintf(L"\n  选择要复制的快捷方式…\n");
    std::wstring source;
    if (!PickFile(source, kLinkFilter, L"选择现有快捷方式")) {
        return;
    }

    std::wprintf(L"\n  原快捷方式：%ls\n", source.c_str());
    if (!Confirm(L"  生成一个通过 UIAccess 启动的副本？")) {
        std::wprintf(L"\n  已取消。\n");
        Pause();
        return;
    }

    std::wstring created;
    std::wstring error;
    if (!CreateUiaShortcut(source, cli, created, error)) {
        std::wprintf(L"\n  创建失败：%ls\n", error.c_str());
        Pause();
        return;
    }

    std::wprintf(L"\n  已创建：%ls\n", created.c_str());
    std::wprintf(L"  目标为 uiauf.exe run，参数、起始位置与图标均沿用原快捷方式。\n");
    Pause();
}

void AddEntry(const std::wstring& cli) {
    std::wprintf(L"\n  输入程序路径（直接回车打开文件选择框）：");
    std::wstring path = ReadLine();
    if (path.empty() && !PickFile(path, kExeFilter, L"选择程序")) {
        return;
    }

    std::wstring hashText;
    DWORD code = 1;
    if (!RunCaptured(cli, L"hash " + uiauf::QuoteArgument(path), hashText, code) || code != 0) {
        std::wprintf(L"\n  无法计算哈希：\n%ls\n", hashText.c_str());
        Pause();
        return;
    }

    const std::wstring digest = Trim(hashText);
    if (!uiauf::IsSha256Hex(digest)) {
        std::wprintf(L"\n  返回的哈希格式异常：%ls\n", digest.c_str());
        Pause();
        return;
    }

    std::wprintf(L"\n  程序：%ls\n", path.c_str());
    std::wprintf(L"  SHA-256：\n    %ls\n", digest.c_str());
    std::wprintf(L"\n  该哈希将写入白名单。之后文件内容一旦变化，启动会被拒绝。\n");
    std::wprintf(L"  请确认这就是你要授权的那个文件。\n\n");

    if (!Confirm(L"  确认注册？")) {
        std::wprintf(L"\n  已取消，未写入任何内容。\n");
        Pause();
        return;
    }

    std::wstring output;
    DWORD exitCode = 1;
    RunCaptured(cli, L"allow add " + uiauf::QuoteArgument(path) + L" --sha256 " + digest, output, exitCode);
    std::wprintf(L"\n%ls\n", output.c_str());
    Pause();
}

void ListEntries(const std::wstring& cli) {
    std::wstring output;
    DWORD code = 1;
    RunCaptured(cli, L"allow list", output, code);
    std::wprintf(L"\n%ls\n", output.c_str());
    Pause();
}

struct Entry {
    std::wstring name;
    std::wstring path;
    std::wstring sha256;
};

std::vector<Entry> LoadEntries(const std::wstring& cli) {
    std::vector<Entry> entries;
    std::wstring listing;
    DWORD code = 1;
    if (!RunCaptured(cli, L"allow list --tsv", listing, code) || code != 0) {
        return entries;
    }

    size_t start = 0;
    while (start < listing.size()) {
        size_t end = listing.find(L'\n', start);
        if (end == std::wstring::npos) {
            end = listing.size();
        }

        const std::wstring line = Trim(listing.substr(start, end - start));
        start = end + 1;
        if (line.empty()) {
            continue;
        }

        const size_t firstTab = line.find(L'\t');
        if (firstTab == std::wstring::npos) {
            continue;
        }
        const size_t secondTab = line.find(L'\t', firstTab + 1);
        if (secondTab == std::wstring::npos) {
            continue;
        }

        Entry entry;
        entry.name = line.substr(0, firstTab);
        entry.path = line.substr(firstTab + 1, secondTab - firstTab - 1);
        entry.sha256 = line.substr(secondTab + 1);
        entries.push_back(std::move(entry));
    }
    return entries;
}

void RemoveEntry(const std::wstring& cli) {
    const std::vector<Entry> entries = LoadEntries(cli);
    if (entries.empty()) {
        std::wprintf(L"\n  白名单为空（或服务未运行）。\n");
        Pause();
        return;
    }

    std::wprintf(L"\n  白名单：\n");
    for (size_t index = 0; index < entries.size(); ++index) {
        std::wprintf(L"    %2zu) %-20ls %ls\n", index + 1, entries[index].name.c_str(),
                     entries[index].path.c_str());
    }

    std::wprintf(L"\n  输入要删除的编号（直接回车取消）：");
    const std::wstring input = ReadLine();
    if (input.empty()) {
        return;
    }

    wchar_t* end = nullptr;
    const unsigned long number = std::wcstoul(input.c_str(), &end, 10);
    if (end == input.c_str() || *end != L'\0' || number == 0 || number > entries.size()) {
        std::wprintf(L"\n  编号无效。\n");
        Pause();
        return;
    }

    const Entry& entry = entries[number - 1];
    std::wprintf(L"\n  条目：%ls\n    程序：%ls\n", entry.name.c_str(), entry.path.c_str());
    if (!Confirm(L"  确认删除？")) {
        std::wprintf(L"\n  已取消。\n");
        Pause();
        return;
    }

    std::wstring output;
    DWORD exitCode = 1;
    RunCaptured(cli, L"allow remove " + uiauf::QuoteArgument(entry.name), output, exitCode);
    std::wprintf(L"\n%ls\n", output.c_str());
    Pause();
}

void RunProgram(const std::wstring& cli) {
    std::wstring path;
    std::wprintf(L"\n  选择要启动的程序…\n");
    if (!PickFile(path, kExeFilter, L"选择程序")) {
        return;
    }

    std::wprintf(L"  启动参数（可留空）：");
    const std::wstring arguments = ReadLine();

    std::wstring command = L"run " + uiauf::QuoteArgument(path);
    if (!arguments.empty()) {
        command += L" " + arguments;
    }

    std::wstring output;
    DWORD exitCode = 1;
    RunCaptured(cli, command, output, exitCode);
    std::wprintf(L"\n%ls\n", output.c_str());
    Pause();
}

void CheckProcess(const std::wstring& cli) {
    std::wprintf(L"\n  输入进程 PID：");
    const std::wstring input = ReadLine();
    if (input.empty()) {
        return;
    }

    std::wstring output;
    DWORD exitCode = 1;
    RunCaptured(cli, L"check " + input, output, exitCode);
    std::wprintf(L"\n%ls\n", output.c_str());
    Pause();
}

} // namespace

int wmain() {
    EnableUnicodeConsole();

    const std::wstring cli = CliPath();
    if (GetFileAttributesW(cli.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"找不到同目录下的 uiauf.exe：%ls\n", cli.c_str());
        return 1;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    for (;;) {
        PrintHeader();
        PrintMenu();

        const std::wstring choice = ReadLine();
        if (choice.empty() || choice == L"0") {
            break;
        }

        if (choice == L"1") {
            AddEntry(cli);
        } else if (choice == L"2") {
            ListEntries(cli);
        } else if (choice == L"3") {
            RemoveEntry(cli);
        } else if (choice == L"4") {
            RunProgram(cli);
        } else if (choice == L"5") {
            CheckProcess(cli);
        } else if (choice == L"6") {
            CreateShortcut(cli);
        }
    }

    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return 0;
}
