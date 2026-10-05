#pragma once

#include "TypeSafeClient.h"

#include <Windows.h>

#include <string>
#include <vector>

// Picks the editor a coding goal ("write hello world program") opens in:
//   1. the most recently focused RUNNING IDE window (top-level Z-order:
//      Cursor, VS Code, Visual Studio, Notepad++, JetBrains IDEs, Sublime,
//      Zed, Windsurf, Android Studio ...) - focused, never a second IDE;
//   2. otherwise the most recently USED installed IDE (newest per-IDE
//      state/session file under %APPDATA% / %LOCALAPPDATA%), resolved to its
//      executable via the launch catalog or its standard install folder;
//   3. otherwise Notepad.
// Only executables whose file name is on the fixed IDE list are ever used.
struct CodeEditorChoice {
    std::wstring exePath;       // validated IDE executable; empty = Notepad
    std::wstring name;          // "VS Code"
    std::wstring args;          // extra arguments before the file ("/Edit" for devenv)
    std::wstring languageHint;  // default language for the model ("Python", "C++")
    std::wstring source;        // running | recent | installed | notepad (for logs)
    HWND window = nullptr;      // running IDE top-level window to focus
};

[[nodiscard]] CodeEditorChoice FindPreferredCodeEditor(const std::vector<LaunchCandidate>& candidates);

// True when exePath exists and its file name is a known IDE executable.
[[nodiscard]] bool IsKnownCodeEditorExe(const std::wstring& exePath);

// Focuses window (when still a visible top-level window of exePath) and opens
// file in that editor: CreateProcess("<exe>" <args> "<file>"); single-instance
// IDEs forward the file to the running window. Empty exePath = Notepad.
// Call on the UI thread while the dock still owns the foreground.
[[nodiscard]] bool OpenFileInCodeEditor(const std::wstring& exePath, const std::wstring& args, HWND window,
    const std::wstring& file);
