#include "DockApp.h"

#include <Windows.h>

#include <exception>
#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        DockApp app(instance);
        return app.Run();
    } catch (const std::exception& exception) {
        const std::string narrow(exception.what());
        const std::wstring message(narrow.begin(), narrow.end());
        MessageBoxW(nullptr, message.c_str(), L"Liquid Glass Dock", MB_ICONERROR | MB_OK);
        return 1;
    }
}
