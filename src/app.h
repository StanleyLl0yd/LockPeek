#pragma once
#include <windows.h>
#include <string>
namespace lockpeek {
int RunApplication(HINSTANCE instance, const std::wstring& initialFile, int show);
}
