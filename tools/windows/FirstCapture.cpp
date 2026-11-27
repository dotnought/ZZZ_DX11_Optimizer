// SPDX-License-Identifier: GPL-3.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t Game[] = L"ZenlessZoneZero.exe";
constexpr wchar_t BetaGame[] = L"ZenlessZoneZeroBeta.exe";
constexpr char StockHash[] = "74a01bc38bbc2f85e4c15b7c22976f5a68ce3686faa2b973ed4e944c7c70976c";
constexpr char PresentMonHash[] = "b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af";
constexpr int Seconds = 45;

struct HandleCloser {
    void operator()(void* handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

[[noreturn]] void WindowsError(const char* action)
{
    throw std::system_error(GetLastError(), std::system_category(), action);
}

fs::path BundlePath()
{
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path))
        throw std::runtime_error("Could not locate the extracted test package.");
    return fs::path(path).parent_path();
}

std::vector<DWORD> FindProcesses(const wchar_t* name, const wchar_t* other = L"")
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        WindowsError("Could not list open applications");
    Handle handle(snapshot);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry))
        WindowsError("Could not read the process list");
    std::vector<DWORD> matches;
    do {
        if (_wcsicmp(entry.szExeFile, name) == 0 || _wcsicmp(entry.szExeFile, other) == 0)
            matches.push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot, &entry));
    return matches;
}

std::string FileHash(const fs::path& path)
{
    // All inputs here are package files, the XXMI DLL, or its verified backup.
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("A required file could not be opened.");
    std::vector<unsigned char> bytes(65536);
    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    struct ReleaseHash {
        BCRYPT_ALG_HANDLE& provider;
        BCRYPT_HASH_HANDLE& hash;
        ~ReleaseHash() {
            if (hash) BCryptDestroyHash(hash);
            if (provider) BCryptCloseAlgorithmProvider(provider, 0);
        }
    } release{provider, hash};
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Windows could not initialize SHA-256.");
    while (input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || input.gcount()) {
        if (BCryptHashData(hash, bytes.data(), static_cast<ULONG>(input.gcount()), 0) < 0)
            throw std::runtime_error("Windows could not hash the file.");
    }
    if (!input.eof() || input.bad())
        throw std::runtime_error("Could not finish reading the file.");
    unsigned char digest[32];
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
        throw std::runtime_error("Windows could not read the SHA-256 digest.");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

fs::path ChooseImporter()
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("Windows could not open the folder picker.");
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Select the ZZMI folder containing d3dx.ini");
    const HRESULT shown = dialog->Show(nullptr);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    ComPtr<IShellItem> item;
    if (FAILED(shown) || FAILED(dialog->GetResult(&item)))
        throw std::runtime_error("Windows could not read the selected folder.");
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
        throw std::runtime_error("Choose a ZZMI folder on a local drive.");
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, &CoTaskMemFree);
    fs::path dir(path.get());
    if (!fs::is_regular_file(dir / L"d3dx.ini") || !fs::is_regular_file(dir / L"d3d11.dll"))
        throw std::runtime_error("That folder needs both d3dx.ini and d3d11.dll. In XXMI, open the ZZMI Mods folder and select its parent.");
    return dir;
}

void CheckInstallFiles(const fs::path& root, const fs::path& dir)
{
    const auto config = dir / L"zzz-profiler.ini";
    if (fs::exists(config) &&
        (GetPrivateProfileIntW(L"Capture", L"Level", 0, config.c_str()) != 2 ||
         GetPrivateProfileIntW(L"Capture", L"Seconds", Seconds, config.c_str()) != Seconds))
        throw std::runtime_error("An existing zzz-profiler.ini uses different settings. Set Level=2 and Seconds=45 in that file before retrying.");
    std::ifstream expected(root / L"bin/d3d11.sha256");
    std::string digest;
    expected >> digest;
    if (digest.empty() || FileHash(root / L"bin/d3d11.dll") != digest)
        throw std::runtime_error("The extracted profiler DLL did not match its checksum. Download and extract the package again.");
}

bool Yes()
{
    std::wstring reply;
    return std::getline(std::wcin, reply) && (reply == L"y" || reply == L"Y");
}

void SwitchDll(const fs::path& root, const fs::path& dir, bool restore)
{
    if (!FindProcesses(Game, BetaGame).empty() || !FindProcesses(L"XXMI Launcher.exe").empty())
        throw std::runtime_error("Close ZZZ and XXMI Launcher, including its tray icon, then try again.");

    const auto dll = dir / L"d3d11.dll";
    const auto backup = dir / L"d3d11.zzz-profiler-stock";
    const auto marker = dir / L"d3d11.zzz-profiler-hash";
    const auto config = dir / L"zzz-profiler.ini";
    const auto staged = dir / (L"zzz-profiler-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    const auto current = FileHash(dll);
    if (restore) {
        if (!fs::exists(backup) || FileHash(backup) != StockHash)
            throw std::runtime_error("The verified backup is missing. Use XXMI Launcher to repair its libraries.");
        if (current != StockHash) {
            std::ifstream input(marker);
            std::string previouslyInstalled;
            input >> previouslyInstalled;
            if (previouslyInstalled != current)
                throw std::runtime_error("The installed DLL changed. Use XXMI Launcher to repair its libraries.");
        }
        std::wcout << L"Restore stock XXMI in " << dir.wstring() << L"? [y/N] " << std::flush;
    } else {
        CheckInstallFiles(root, dir);
        if (current != StockHash)
            throw std::runtime_error("This folder does not contain the official XXMI Libraries v1.3.3 DLL. Keep the vanilla results and tell Codex which XXMI Libraries version you have.");
        if (fs::exists(backup) || fs::exists(marker))
            throw std::runtime_error("Previous install files were found. Choose Restore stock XXMI first.");
        std::wcout << L"Back up stock XXMI and install the Level 2 profiler in " << dir.wstring() << L"? [y/N] " << std::flush;
    }
    if (!Yes())
        return;

    std::error_code ignored;
    bool wroteMarker = false;
    bool wroteConfig = false;
    try {
        if (!restore) {
            if (!fs::exists(config)) {
                fs::copy_file(root / L"config/zzz-profiler.ini", config);
                wroteConfig = true;
            }
            std::ofstream hashFile(marker);
            wroteMarker = true;
            hashFile << FileHash(root / L"bin/d3d11.dll") << '\n';
            hashFile.close();
            if (!hashFile)
                throw std::runtime_error("Could not write the rollback information.");
        }
        if (!restore || current != StockHash) {
            fs::copy_file(restore ? backup : root / L"bin/d3d11.dll", staged);
            if (!ReplaceFileW(dll.c_str(), staged.c_str(), restore ? nullptr : backup.c_str(), 0, nullptr, nullptr))
                WindowsError("Could not replace d3d11.dll");
        }
    } catch (...) {
        fs::remove(staged, ignored);
        if (wroteMarker && !fs::exists(backup))
            fs::remove(marker, ignored);
        if (wroteConfig && !fs::exists(backup))
            fs::remove(config, ignored);
        throw;
    }
    if (restore) {
        fs::remove(marker, ignored);
        fs::remove(backup, ignored);
        std::wcout << L"Stock XXMI restored. Return Unsafe Mode to its previous setting in XXMI Launcher.\n";
    } else {
        std::wcout << L"Profiler installed. Open XXMI Launcher, choose ZZMI and check Settings > Advanced > Unsafe Mode before launching.\n";
    }
}

std::wstring RunName(DWORD pid, const wchar_t* scene, const wchar_t* condition)
{
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t stamp[64];
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u-%03u-%lu",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds, pid);
    return std::wstring(stamp) + L"-" + scene + L"-" + condition;
}

DWORD Record(const fs::path& exe, const fs::path& run, DWORD pid, const std::wstring& name)
{
    std::wstring args = L"\"" + exe.wstring() + L"\" --process_id " + std::to_wstring(pid) +
        L" --output_file \"" + (run / L"presentmon.csv").wstring() +
        L"\" --session_name \"ZZZ-" + name + L"\" --hotkey CTRL+ALT+F8 --timed " +
        std::to_wstring(Seconds) + L" --terminate_after_timed --terminate_on_proc_exit"
        L" --qpc_time --no_console_stats --no_track_input";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, FALSE, 0, nullptr, run.c_str(), &si, &pi))
        WindowsError("Could not start PresentMon");
    Handle process(pi.hProcess);
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0)
        WindowsError("Could not wait for PresentMon");
    DWORD status = 0;
    if (!GetExitCodeProcess(process.get(), &status))
        WindowsError("Could not check PresentMon's result");
    return status;
}

std::string CopyNative(const fs::path& dir, const fs::path& run, DWORD pid, fs::file_time_type after)
{
    const auto captures = dir / L"zzz-captures";
    const auto marker = L"-" + std::to_wstring(pid) + L"-";
    for (int attempt = 0; attempt < 24; ++attempt) {
        if (fs::is_directory(captures)) {
            for (const auto& entry : fs::directory_iterator(captures)) {
                const std::wstring file = entry.path().filename().wstring();
                if (entry.path().extension() != L".json" || file.find(L"capture-") != 0 ||
                    file.find(marker) == std::wstring::npos || entry.last_write_time() < after)
                    continue;
                const auto stem = entry.path().stem().wstring();
                for (const auto suffix : {L"-frames.csv", L"-apis.csv", L"-gpu.csv", L"-memory.csv", L".json"}) {
                    const auto part = stem + suffix;
                    fs::copy_file(captures / part, run / part);
                }
                return entry.path().filename().string();
            }
        }
        Sleep(500);
    }
    return {};
}

void Capture(const fs::path& root, const fs::path& dir, const std::string& session, bool roscaelifer, bool native)
{
    const auto games = FindProcesses(Game, BetaGame);
    if (games.size() != 1)
        throw std::runtime_error("Open one ZZZ game window, enter the area you want to capture, then choose the recording again.");
    if (native) {
        if (FileHash(dir / L"d3d11.dll") != FileHash(root / L"bin/d3d11.dll"))
            throw std::runtime_error("XXMI is using a different DLL. Close ZZZ and XXMI, then use the install option and check the launcher setting in the guide.");
        CheckInstallFiles(root, dir);
        if (!fs::exists(dir / L"zzz-profiler.ini"))
            throw std::runtime_error("The native profiler needs zzz-profiler.ini. Close ZZZ and XXMI, then restore and reinstall.");
    }
    const auto exe = root / L"third-party/PresentMon/PresentMon-2.6.0-x64.exe";
    if (FileHash(exe) != PresentMonHash)
        throw std::runtime_error("PresentMon did not match its checksum. Download and extract the package again.");
    const wchar_t* area = roscaelifer ? L"Roscaelifer" : L"SixthStreet";
    const wchar_t* variant = native ? L"profiler-l2" : L"vanilla";
    const auto name = RunName(games.front(), area, variant);
    const auto run = root / L"runs" / name;
    fs::create_directories(run);
    const auto armed = fs::file_time_type::clock::now();
    std::wcout << L"Waiting for Ctrl+Alt+F8. Alt+Tab to ZZZ and hold the chord briefly.\n"
               << L"Keep the game in the foreground for 45 seconds, then check this window.\n" << std::flush;
    if (Record(exe, run, games.front(), name) != 0 || !fs::exists(run / L"presentmon.csv"))
        throw std::runtime_error("PresentMon could not record this run. Check the error printed above, then try again.");
    std::ifstream csv(run / L"presentmon.csv");
    std::string row;
    if (!std::getline(csv, row) || !std::getline(csv, row) || row.empty())
        throw std::runtime_error("PresentMon recorded no frames. Return to ZZZ before using Ctrl+Alt+F8 and keep the game open for the whole run.");
    const auto captured = native ? CopyNative(dir, run, games.front(), armed) : std::string{};
    std::ofstream report(run / L"run.json");
    report << "{\n  \"scene\": \"" << (roscaelifer ? "Roscaelifer" : "SixthStreet")
           << "\",\n  \"condition\": \"" << (native ? "profiler-l2" : "vanilla")
           << "\",\n  \"settings_id\": \"" << session << "\",\n  \"game_version\": \"see-session-notes\","
              "\n  \"driver\": \"see-session-notes\",\n  \"route\": \"fixed-camera\",\n  \"cache\": \"warm\","
              "\n  \"planned_seconds\": " << Seconds << ",\n  \"pid\": " << games.front()
           << ",\n  \"presentmon\": \"presentmon.csv\"";
    if (!captured.empty())
        report << ",\n  \"native\": \"" << captured << '"';
    else if (native)
        report << ",\n  \"native_missing\": true";
    report << "\n}\n";
    report.close();
    if (!report)
        throw std::runtime_error("Could not write run.json.");
    if (native && captured.empty())
        std::wcout << L"Saved PresentMon data. The native file did not arrive. Keep this run and check the guide.\n";
    std::wcout << L"Saved " << run.wstring() << L"\n";
}
}

int wmain()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        std::cerr << "Windows could not start the capture menu.\n";
        return 1;
    }
    try {
        const auto root = BundlePath();
        const auto id = RunName(GetCurrentProcessId(), L"guided", L"first");
        const std::string session(id.begin(), id.end()); // RunName contains ASCII only.
        fs::path importer;
        for (;;) {
            std::wcout << L"\nZZZ DX11 first capture\n"
                L"1  Record Vanilla - Roscaelifer\n2  Record Vanilla - Sixth Street\n"
                L"3  Install the profiler in ZZMI\n"
                L"4  Record Level 2 - Roscaelifer\n5  Record Level 2 - Sixth Street\n"
                L"6  Restore stock XXMI\n0  Exit\nChoice: " << std::flush;
            std::wstring choice;
            if (!std::getline(std::wcin, choice) || choice == L"0")
                break;
            try {
                if (choice == L"1" || choice == L"2") {
                    Capture(root, {}, session, choice == L"1", false);
                } else if (choice == L"3" || choice == L"4" || choice == L"5" || choice == L"6") {
                    if (importer.empty()) {
                        importer = ChooseImporter();
                        if (importer.empty())
                            continue;
                    }
                    if (choice == L"3" || choice == L"6")
                        SwitchDll(root, importer, choice == L"6");
                    else
                        Capture(root, importer, session, choice == L"4", true);
                } else {
                    std::wcout << L"Choose a number from the list.\n";
                }
            } catch (const std::exception& error) {
                // Keep the console open so the user can read the error and retry.
                std::wcerr << L"\n" << error.what() << L"\n";
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    return 0;
}
