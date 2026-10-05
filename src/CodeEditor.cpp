#include "CodeEditor.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

struct IdeInfo {
    std::wstring_view exe;    // lowercase file name
    std::wstring_view name;
    std::wstring_view language;
    std::wstring_view args;
    // '|'-separated, environment variables expanded, '*' globs a path segment.
    std::wstring_view stateFiles;
    std::wstring_view installPaths;
};

// Order is the tie-break when no recency signal exists.
constexpr IdeInfo kIdes[] = {
    {L"cursor.exe", L"Cursor", L"Python", L"",
        L"%APPDATA%\\Cursor\\User\\globalStorage\\state.vscdb|%APPDATA%\\Cursor\\User\\globalStorage\\storage.json",
        L"%LOCALAPPDATA%\\Programs\\cursor\\Cursor.exe"},
    {L"code.exe", L"VS Code", L"Python", L"",
        L"%APPDATA%\\Code\\User\\globalStorage\\state.vscdb|%APPDATA%\\Code\\User\\globalStorage\\storage.json",
        L"%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe|%ProgramFiles%\\Microsoft VS Code\\Code.exe"},
    {L"code - insiders.exe", L"VS Code Insiders", L"Python", L"",
        L"%APPDATA%\\Code - Insiders\\User\\globalStorage\\state.vscdb|"
        L"%APPDATA%\\Code - Insiders\\User\\globalStorage\\storage.json",
        L"%LOCALAPPDATA%\\Programs\\Microsoft VS Code Insiders\\Code - Insiders.exe"},
    {L"windsurf.exe", L"Windsurf", L"Python", L"",
        L"%APPDATA%\\Windsurf\\User\\globalStorage\\state.vscdb|%APPDATA%\\Windsurf\\User\\globalStorage\\storage.json",
        L"%LOCALAPPDATA%\\Programs\\Windsurf\\Windsurf.exe"},
    {L"devenv.exe", L"Visual Studio", L"C++", L"/Edit",
        L"%LOCALAPPDATA%\\Microsoft\\VisualStudio\\*\\ApplicationPrivateSettings.xml",
        L"%ProgramFiles%\\Microsoft Visual Studio\\*\\*\\Common7\\IDE\\devenv.exe|"
        L"%ProgramFiles(x86)%\\Microsoft Visual Studio\\*\\*\\Common7\\IDE\\devenv.exe"},
    {L"notepad++.exe", L"Notepad++", L"Python", L"",
        L"%APPDATA%\\Notepad++\\session.xml|%APPDATA%\\Notepad++\\config.xml",
        L"%ProgramFiles%\\Notepad++\\notepad++.exe|%ProgramFiles(x86)%\\Notepad++\\notepad++.exe"},
    {L"pycharm64.exe", L"PyCharm", L"Python", L"",
        L"%APPDATA%\\JetBrains\\PyCharm*\\options\\recentProjects.xml",
        L"%ProgramFiles%\\JetBrains\\*\\bin\\pycharm64.exe|%LOCALAPPDATA%\\Programs\\PyCharm*\\bin\\pycharm64.exe"},
    {L"idea64.exe", L"IntelliJ IDEA", L"Java", L"",
        L"%APPDATA%\\JetBrains\\IntelliJIdea*\\options\\recentProjects.xml|"
        L"%APPDATA%\\JetBrains\\IdeaIC*\\options\\recentProjects.xml",
        L"%ProgramFiles%\\JetBrains\\*\\bin\\idea64.exe|%LOCALAPPDATA%\\Programs\\IntelliJ*\\bin\\idea64.exe"},
    {L"clion64.exe", L"CLion", L"C++", L"",
        L"%APPDATA%\\JetBrains\\CLion*\\options\\recentProjects.xml",
        L"%ProgramFiles%\\JetBrains\\*\\bin\\clion64.exe|%LOCALAPPDATA%\\Programs\\CLion*\\bin\\clion64.exe"},
    {L"webstorm64.exe", L"WebStorm", L"JavaScript", L"",
        L"%APPDATA%\\JetBrains\\WebStorm*\\options\\recentProjects.xml",
        L"%ProgramFiles%\\JetBrains\\*\\bin\\webstorm64.exe|%LOCALAPPDATA%\\Programs\\WebStorm*\\bin\\webstorm64.exe"},
    {L"rider64.exe", L"Rider", L"C#", L"",
        L"%APPDATA%\\JetBrains\\Rider*\\options\\recentProjects.xml|"
        L"%APPDATA%\\JetBrains\\Rider*\\options\\recentSolutions.xml",
        L"%ProgramFiles%\\JetBrains\\*\\bin\\rider64.exe|%LOCALAPPDATA%\\Programs\\Rider*\\bin\\rider64.exe"},
    {L"goland64.exe", L"GoLand", L"Go", L"",
        L"%APPDATA%\\JetBrains\\GoLand*\\options\\recentProjects.xml",
        L"%ProgramFiles%\\JetBrains\\*\\bin\\goland64.exe|%LOCALAPPDATA%\\Programs\\GoLand*\\bin\\goland64.exe"},
    {L"rustrover64.exe", L"RustRover", L"Rust", L"",
        L"%APPDATA%\\JetBrains\\RustRover*\\options\\recentProjects.xml",
        L"%ProgramFiles%\\JetBrains\\*\\bin\\rustrover64.exe|%LOCALAPPDATA%\\Programs\\RustRover*\\bin\\rustrover64.exe"},
    {L"phpstorm64.exe", L"PhpStorm", L"JavaScript", L"",
        L"%APPDATA%\\JetBrains\\PhpStorm*\\options\\recentProjects.xml",
        L"%ProgramFiles%\\JetBrains\\*\\bin\\phpstorm64.exe|%LOCALAPPDATA%\\Programs\\PhpStorm*\\bin\\phpstorm64.exe"},
    {L"studio64.exe", L"Android Studio", L"Kotlin", L"",
        L"%APPDATA%\\Google\\AndroidStudio*\\options\\recentProjects.xml",
        L"%ProgramFiles%\\Android\\Android Studio\\bin\\studio64.exe"},
    {L"sublime_text.exe", L"Sublime Text", L"Python", L"",
        L"%APPDATA%\\Sublime Text\\Local\\Session.sublime_session|"
        L"%APPDATA%\\Sublime Text 3\\Local\\Session.sublime_session",
        L"%ProgramFiles%\\Sublime Text\\sublime_text.exe|%ProgramFiles%\\Sublime Text 3\\sublime_text.exe"},
    {L"zed.exe", L"Zed", L"Python", L"", L"", L"%LOCALAPPDATA%\\Programs\\Zed\\zed.exe"},
};

std::wstring Lower(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring FileNameLower(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    return Lower(sep == std::wstring::npos ? path : path.substr(sep + 1));
}

const IdeInfo* IdeByExe(const std::wstring& path) {
    const std::wstring name = FileNameLower(path);
    for (const IdeInfo& ide : kIdes) {
        if (name == ide.exe) {
            return &ide;
        }
    }
    return nullptr;
}

bool IsRegularFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring Expand(std::wstring_view raw) {
    const std::wstring source(raw);
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD length = ExpandEnvironmentStringsW(source.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length > std::size(buffer) || std::wstring_view(buffer).find(L'%') != std::wstring_view::npos) {
        return {};  // unknown variable (e.g. no ProgramFiles(x86)) or too long
    }
    return buffer;
}

std::vector<std::wstring> SplitBar(std::wstring_view list) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start < list.size()) {
        size_t end = list.find(L'|', start);
        if (end == std::wstring_view::npos) {
            end = list.size();
        }
        if (end > start) {
            parts.emplace_back(list.substr(start, end - start));
        }
        start = end + 1;
    }
    return parts;
}

// Expands '*' in path segments ("C:\A\*\B\x.exe"); bounded fan-out.
void Glob(const std::wstring& pattern, std::vector<std::wstring>& out, int depth = 0) {
    if (depth > 4 || out.size() > 64) {
        return;
    }
    const size_t star = pattern.find(L'*');
    if (star == std::wstring::npos) {
        if (GetFileAttributesW(pattern.c_str()) != INVALID_FILE_ATTRIBUTES) {
            out.push_back(pattern);
        }
        return;
    }
    const size_t segStart = pattern.rfind(L'\\', star);
    const size_t segEnd = pattern.find(L'\\', star);
    if (segStart == std::wstring::npos) {
        return;
    }
    const std::wstring parent = pattern.substr(0, segStart);
    const std::wstring segment = pattern.substr(0, segEnd == std::wstring::npos ? pattern.size() : segEnd);
    const std::wstring rest = segEnd == std::wstring::npos ? std::wstring{} : pattern.substr(segEnd);
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(segment.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!rest.empty() && !isDir) {
            continue;
        }
        Glob(parent + L"\\" + data.cFileName + rest, out, depth + 1);
    } while (FindNextFileW(find, &data) != FALSE && out.size() <= 64);
    FindClose(find);
}

ULONGLONG LastWriteTicks(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) {
        return 0;
    }
    return (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

ULONGLONG NewestStateTicks(const IdeInfo& ide) {
    ULONGLONG newest = 0;
    for (const std::wstring& raw : SplitBar(ide.stateFiles)) {
        const std::wstring pattern = Expand(raw);
        if (pattern.empty()) {
            continue;
        }
        std::vector<std::wstring> files;
        Glob(pattern, files);
        for (const std::wstring& file : files) {
            newest = (std::max)(newest, LastWriteTicks(file));
        }
    }
    return newest;
}

std::wstring ResolveInstalledExe(const IdeInfo& ide, const std::vector<LaunchCandidate>& candidates) {
    for (const LaunchCandidate& candidate : candidates) {
        for (const std::wstring* path : {&candidate.executablePath, &candidate.target}) {
            if (!path->empty() && FileNameLower(*path) == ide.exe && IsRegularFile(*path)) {
                return *path;
            }
        }
    }
    for (const std::wstring& raw : SplitBar(ide.installPaths)) {
        const std::wstring pattern = Expand(raw);
        if (pattern.empty()) {
            continue;
        }
        std::vector<std::wstring> found;
        Glob(pattern, found);
        // Newest install wins when several versions are side by side.
        std::wstring best;
        ULONGLONG bestTicks = 0;
        for (const std::wstring& path : found) {
            if (FileNameLower(path) == ide.exe && IsRegularFile(path)) {
                const ULONGLONG ticks = LastWriteTicks(path);
                if (best.empty() || ticks > bestTicks) {
                    best = path;
                    bestTicks = ticks;
                }
            }
        }
        if (!best.empty()) {
            return best;
        }
    }
    return {};
}

std::wstring ProcessImagePath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    std::wstring path;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size) != FALSE) {
        path.assign(buffer, size);
    }
    CloseHandle(process);
    return path;
}

bool IsUserTopLevelWindow(HWND window) {
    if (IsWindowVisible(window) == FALSE || GetWindow(window, GW_OWNER) != nullptr ||
        GetWindowTextLengthW(window) == 0) {
        return false;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }
    return true;
}

struct RunningIde {
    const IdeInfo* ide = nullptr;
    std::wstring exePath;
    HWND window = nullptr;
};

// EnumWindows walks top-level windows in Z-order (most recently activated
// first), so the first IDE window found is the most recently focused IDE.
std::optional<RunningIde> TopmostRunningIde() {
    struct Context {
        DWORD selfPid = GetCurrentProcessId();
        std::unordered_map<DWORD, std::wstring> images;
        std::optional<RunningIde> found;
    } context;
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto* ctx = reinterpret_cast<Context*>(param);
            if (!IsUserTopLevelWindow(window)) {
                return TRUE;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == 0 || pid == ctx->selfPid) {
                return TRUE;
            }
            auto it = ctx->images.find(pid);
            if (it == ctx->images.end()) {
                it = ctx->images.emplace(pid, ProcessImagePath(pid)).first;
            }
            if (const IdeInfo* ide = IdeByExe(it->second); ide != nullptr) {
                ctx->found = RunningIde{ide, it->second, window};
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
    return context.found;
}

std::wstring NotepadPath() {
    wchar_t system[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
    if (length == 0 || length >= std::size(system)) {
        return L"C:\\Windows\\System32\\notepad.exe";
    }
    return std::wstring(system, length) + L"\\notepad.exe";
}

}  // namespace

CodeEditorChoice FindPreferredCodeEditor(const std::vector<LaunchCandidate>& candidates) {
    CodeEditorChoice choice;
    if (const std::optional<RunningIde> running = TopmostRunningIde(); running.has_value()) {
        choice.exePath = running->exePath;
        choice.name = std::wstring(running->ide->name);
        choice.args = std::wstring(running->ide->args);
        choice.languageHint = std::wstring(running->ide->language);
        choice.window = running->window;
        choice.source = L"running";
        return choice;
    }
    const IdeInfo* best = nullptr;
    std::wstring bestExe;
    ULONGLONG bestTicks = 0;
    for (const IdeInfo& ide : kIdes) {
        const ULONGLONG ticks = NewestStateTicks(ide);
        if (best != nullptr && ticks <= bestTicks) {
            continue;  // table order breaks ties (incl. no recency signal)
        }
        std::wstring exe = ResolveInstalledExe(ide, candidates);
        if (exe.empty()) {
            continue;
        }
        best = &ide;
        bestExe = std::move(exe);
        bestTicks = ticks;
    }
    if (best != nullptr) {
        choice.exePath = bestExe;
        choice.name = std::wstring(best->name);
        choice.args = std::wstring(best->args);
        choice.languageHint = std::wstring(best->language);
        choice.source = bestTicks != 0 ? L"recent" : L"installed";
        return choice;
    }
    choice.name = L"Notepad";
    choice.languageHint = L"Python";
    choice.source = L"notepad";
    return choice;
}

bool IsKnownCodeEditorExe(const std::wstring& exePath) {
    return !exePath.empty() && IdeByExe(exePath) != nullptr && IsRegularFile(exePath) &&
        !exePath.starts_with(L"\\\\");
}

bool OpenFileInCodeEditor(const std::wstring& exePath, const std::wstring& args, HWND window,
    const std::wstring& file) {
    std::wstring exe = exePath;
    std::wstring extra = args;
    if (exe.empty()) {
        exe = NotepadPath();
        extra.clear();
        window = nullptr;
    } else if (!IsKnownCodeEditorExe(exe)) {
        return false;
    }
    if (extra != L"" && extra != L"/Edit") {
        return false;  // only the fixed per-IDE arguments
    }
    // Focus the running IDE first while the dock still owns the foreground,
    // then let the editor (or the instance it forwards to) raise itself too.
    AllowSetForegroundWindow(ASFW_ANY);
    if (window != nullptr && IsWindow(window) != FALSE) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != 0 && Lower(ProcessImagePath(pid)) == Lower(exe)) {
            if (IsIconic(window) != FALSE) {
                ShowWindow(window, SW_RESTORE);
            }
            SetForegroundWindow(window);
        }
    }
    std::wstring commandLine = L"\"" + exe + L"\" ";
    if (!extra.empty()) {
        commandLine += extra + L" ";
    }
    commandLine += L"\"" + file + L"\"";
    std::wstring workDir = file.substr(0, file.find_last_of(L'\\'));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
            nullptr, workDir.empty() ? nullptr : workDir.c_str(), &startup, &process) == FALSE) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
