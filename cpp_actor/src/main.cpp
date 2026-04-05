#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <strsafe.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "resources.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr UINT WM_APP_STATUS = WM_APP + 1;
constexpr UINT WM_APP_PROGRESS = WM_APP + 2;
constexpr UINT WM_APP_FINISH = WM_APP + 3;
constexpr UINT TIMER_COUNTDOWN = 1;

constexpr int IDC_PATH = 1001;
constexpr int IDC_BROWSE = 1002;
constexpr int IDC_LOAD = 1003;
constexpr int IDC_START = 1004;
constexpr int IDC_PAUSE = 1005;
constexpr int IDC_STOP = 1006;
constexpr int IDC_DELAY = 1007;
constexpr int IDC_PROGRESS = 1008;
constexpr int IDC_EVENTS = 1009;
constexpr int IDC_EVENT_TYPE = 1010;
constexpr int IDC_STATUS = 1011;
constexpr int IDC_STATE = 1012;
constexpr int IDC_LOG = 1013;
constexpr int IDC_GAMEPAD_SLOT = 1014;

constexpr int HOTKEY_ID_START = 1;
constexpr int HOTKEY_ID_PAUSE = 2;
constexpr int HOTKEY_ID_STOP = 3;

constexpr wchar_t DEFAULT_START_HOTKEY[] = L"Ctrl+Alt+F6";
constexpr wchar_t DEFAULT_PAUSE_HOTKEY[] = L"Ctrl+Alt+F7";
constexpr wchar_t DEFAULT_STOP_HOTKEY[] = L"Ctrl+Alt+F8";

constexpr int MAX_XINPUT_GAMEPAD_INDEX = 3;

using SteadyClock = std::chrono::steady_clock;

std::wstring WideFromUtf8(const std::string& input) {
    if (input.empty()) {
        return L"";
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) {
        return L"";
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), out.data(), size);
    return out;
}

std::string Utf8FromWide(const std::wstring& input) {
    if (input.empty()) {
        return "";
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return "";
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring FormatError(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf),
        0,
        nullptr);

    std::wstring msg = L"Win32 error " + std::to_wstring(code);
    if (buf != nullptr) {
        msg = buf;
        LocalFree(buf);
    }
    return msg;
}

std::wstring TimeStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64]{};
    StringCchPrintfW(buf, 64, L"[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

fs::path EmbeddedWorkDir() {
    wchar_t local_app_data[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    fs::path base;
    if (len > 0 && len < MAX_PATH) {
        base = fs::path(local_app_data);
    } else {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        base = fs::path(temp);
    }
    return base / L"InputActorCpp" / L"embedded";
}

bool ExtractEmbeddedResourceToFile(int resource_id, const fs::path& out_path, std::wstring& error) {
    HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        error = L"GetModuleHandle failed: " + FormatError(GetLastError());
        return false;
    }

    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (resource == nullptr) {
        error = L"Embedded resource not found: id=" + std::to_wstring(resource_id);
        return false;
    }

    DWORD size = SizeofResource(module, resource);
    HGLOBAL loaded = LoadResource(module, resource);
    if (loaded == nullptr || size == 0) {
        error = L"Failed to load embedded resource id=" + std::to_wstring(resource_id);
        return false;
    }

    const void* data = LockResource(loaded);
    if (data == nullptr) {
        error = L"Failed to lock embedded resource id=" + std::to_wstring(resource_id);
        return false;
    }

    std::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);
    if (ec) {
        error = L"Failed to create directory: " + out_path.parent_path().wstring();
        return false;
    }

    // Skip rewrite when size matches to reduce churn and file locking risk.
    if (fs::exists(out_path, ec) && !ec) {
        uintmax_t existing_size = fs::file_size(out_path, ec);
        if (!ec && existing_size == size) {
            return true;
        }
    }

    fs::path temp_path = out_path;
    temp_path += L".tmp";
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = L"Failed to open temp file for writing: " + temp_path.wstring();
            return false;
        }
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out.good()) {
            error = L"Failed to write embedded resource to: " + temp_path.wstring();
            return false;
        }
    }

    fs::rename(temp_path, out_path, ec);
    if (ec) {
        fs::remove(out_path, ec);
        ec.clear();
        fs::rename(temp_path, out_path, ec);
    }

    if (ec) {
        error = L"Failed to finalize extracted file: " + out_path.wstring();
        return false;
    }
    return true;
}

std::optional<fs::path> ExtractEmbeddedResource(int resource_id, const std::wstring& filename, std::wstring& error) {
    const fs::path out = EmbeddedWorkDir() / filename;
    if (!ExtractEmbeddedResourceToFile(resource_id, out, error)) {
        return std::nullopt;
    }
    return out;
}

std::string LowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::wstring LowerCopy(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return text;
}

int64_t GetInt64(const json& value, int64_t fallback = 0) {
    try {
        if (value.is_number_integer()) {
            return value.get<int64_t>();
        }
        if (value.is_number_unsigned()) {
            return static_cast<int64_t>(value.get<uint64_t>());
        }
        if (value.is_number_float()) {
            return static_cast<int64_t>(value.get<double>());
        }
        if (value.is_string()) {
            return std::stoll(value.get<std::string>());
        }
    } catch (...) {
        return fallback;
    }
    return fallback;
}

double GetDouble(const json& value, double fallback = 0.0) {
    try {
        if (value.is_number_float()) {
            return value.get<double>();
        }
        if (value.is_number_integer()) {
            return static_cast<double>(value.get<int64_t>());
        }
        if (value.is_number_unsigned()) {
            return static_cast<double>(value.get<uint64_t>());
        }
        if (value.is_string()) {
            return std::stod(value.get<std::string>());
        }
    } catch (...) {
        return fallback;
    }
    return fallback;
}

struct SessionMeta {
    int64_t qpc_freq = 10'000'000;
};

struct ReplayEvent {
    std::string type;
    int64_t t_qpc = 0;
    json data;
};

bool LoadJsonl(const fs::path& path, SessionMeta& meta, std::vector<ReplayEvent>& events, std::wstring& error) {
    events.clear();
    meta = SessionMeta{};

    if (!fs::exists(path)) {
        error = L"File not found: " + path.wstring();
        return false;
    }
    if (LowerCopy(path.extension().wstring()) != L".jsonl") {
        error = L"Input file must use .jsonl extension.";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = L"Unable to open file.";
        return false;
    }

    std::string line;
    size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        json obj;
        try {
            obj = json::parse(line);
        } catch (const std::exception& ex) {
            error = L"Invalid JSON at line " + std::to_wstring(line_number) + L": " + WideFromUtf8(ex.what());
            return false;
        }

        if (!obj.is_object()) {
            continue;
        }

        std::string type = LowerCopy(obj.value("type", std::string{}));
        if (type == "session_header") {
            meta.qpc_freq = std::max<int64_t>(1, GetInt64(obj.value("qpc_freq", 10'000'000), 10'000'000));
            continue;
        }
        if (type == "stats") {
            continue;
        }
        if (!obj.contains("t_qpc")) {
            continue;
        }

        ReplayEvent ev{};
        ev.type = type;
        ev.t_qpc = GetInt64(obj["t_qpc"]);
        ev.data = std::move(obj);
        events.push_back(std::move(ev));
    }

    if (events.empty()) {
        error = L"No replayable events were found in the file.";
        return false;
    }
    return true;
}

using VIGEM_ERROR = int32_t;
using PVIGEM_CLIENT = void*;
using PVIGEM_TARGET = void*;

constexpr VIGEM_ERROR VIGEM_ERROR_NONE = 0x20000000;
constexpr VIGEM_ERROR VIGEM_ERROR_ALREADY_CONNECTED = static_cast<VIGEM_ERROR>(0xE0000005);

enum XUSB_BUTTON : uint16_t {
    XUSB_GAMEPAD_DPAD_UP = 0x0001,
    XUSB_GAMEPAD_DPAD_DOWN = 0x0002,
    XUSB_GAMEPAD_DPAD_LEFT = 0x0004,
    XUSB_GAMEPAD_DPAD_RIGHT = 0x0008,
    XUSB_GAMEPAD_START = 0x0010,
    XUSB_GAMEPAD_BACK = 0x0020,
    XUSB_GAMEPAD_LEFT_THUMB = 0x0040,
    XUSB_GAMEPAD_RIGHT_THUMB = 0x0080,
    XUSB_GAMEPAD_LEFT_SHOULDER = 0x0100,
    XUSB_GAMEPAD_RIGHT_SHOULDER = 0x0200,
    XUSB_GAMEPAD_GUIDE = 0x0400,
    XUSB_GAMEPAD_A = 0x1000,
    XUSB_GAMEPAD_B = 0x2000,
    XUSB_GAMEPAD_X = 0x4000,
    XUSB_GAMEPAD_Y = 0x8000,
};

struct XUSB_REPORT {
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};

class VigemRuntime {
public:
    ~VigemRuntime() {
        Reset();
    }

    bool EnsureReady(std::wstring& error) {
        if (ready_) {
            return true;
        }

        if (!LoadClientDll(error)) {
            if (!InstallDriver(error)) {
                return false;
            }
            if (!LoadClientDll(error)) {
                return false;
            }
        }

        if (!ResolveApi(error)) {
            return false;
        }

        client_ = api_.alloc();
        if (client_ == nullptr) {
            error = L"vigem_alloc failed.";
            return false;
        }

        VIGEM_ERROR result = api_.connect(client_);
        if (result != VIGEM_ERROR_NONE) {
            api_.free(client_);
            client_ = nullptr;
            error = L"vigem_connect failed with error code " + std::to_wstring(result) + L".";
            return false;
        }

        ready_ = true;
        return true;
    }

    void Reset() {
        if (!ready_) {
            if (dll_ != nullptr) {
                FreeLibrary(dll_);
                dll_ = nullptr;
            }
            return;
        }

        for (const auto& [index, target] : targets_) {
            XUSB_REPORT zero{};
            api_.x360_update(client_, target, zero);
            api_.target_remove(client_, target);
            api_.target_free(target);
        }
        targets_.clear();
        reports_.clear();

        api_.disconnect(client_);
        api_.free(client_);
        client_ = nullptr;
        ready_ = false;

        if (dll_ != nullptr) {
            FreeLibrary(dll_);
            dll_ = nullptr;
        }
    }

    bool Send(const ReplayEvent& event, std::wstring& error) {
        if (!EnsureReady(error)) {
            return false;
        }

        int index = static_cast<int>(GetInt64(event.data.value("gamepad_index", 0), 0));
        if (index < 0 || index > MAX_XINPUT_GAMEPAD_INDEX) {
            error = L"Unsupported gamepad_index: " + std::to_wstring(index);
            return false;
        }

        if (!EnsureTarget(index, error)) {
            return false;
        }

        XUSB_REPORT& report = reports_[index];
        PVIGEM_TARGET target = targets_[index];

        if (event.type == "gamepad_connected" || event.type == "gamepad_disconnected") {
            report = XUSB_REPORT{};
            return PushReport(target, report, error);
        }

        if (event.type == "gamepad_button_down" || event.type == "gamepad_button_up") {
            bool down = (event.type == "gamepad_button_down");
            std::string control = LowerCopy(event.data.value("control", std::string{}));
            std::replace(control.begin(), control.end(), '-', '_');
            std::replace(control.begin(), control.end(), ' ', '_');
            std::replace(control.begin(), control.end(), '.', '_');

            const std::unordered_map<std::string, std::string> aliases{
                {"lb", "left_shoulder"},
                {"rb", "right_shoulder"},
                {"ls", "left_thumb"},
                {"rs", "right_thumb"},
                {"select", "back"},
                {"view", "back"},
                {"menu", "start"},
                {"options", "start"},
                {"left_stick_press", "left_thumb"},
                {"right_stick_press", "right_thumb"},
            };
            if (auto it = aliases.find(control); it != aliases.end()) {
                control = it->second;
            }

            const std::unordered_map<std::string, uint16_t> buttons{
                {"a", XUSB_GAMEPAD_A},
                {"b", XUSB_GAMEPAD_B},
                {"x", XUSB_GAMEPAD_X},
                {"y", XUSB_GAMEPAD_Y},
                {"dpad_up", XUSB_GAMEPAD_DPAD_UP},
                {"dpad_down", XUSB_GAMEPAD_DPAD_DOWN},
                {"dpad_left", XUSB_GAMEPAD_DPAD_LEFT},
                {"dpad_right", XUSB_GAMEPAD_DPAD_RIGHT},
                {"start", XUSB_GAMEPAD_START},
                {"back", XUSB_GAMEPAD_BACK},
                {"guide", XUSB_GAMEPAD_GUIDE},
                {"left_shoulder", XUSB_GAMEPAD_LEFT_SHOULDER},
                {"right_shoulder", XUSB_GAMEPAD_RIGHT_SHOULDER},
                {"left_thumb", XUSB_GAMEPAD_LEFT_THUMB},
                {"right_thumb", XUSB_GAMEPAD_RIGHT_THUMB},
            };

            auto bit = buttons.find(control);
            if (bit == buttons.end()) {
                error = L"Unsupported gamepad button control: " + WideFromUtf8(control);
                return false;
            }

            if (down) {
                report.wButtons = static_cast<uint16_t>(report.wButtons | bit->second);
            } else {
                report.wButtons = static_cast<uint16_t>(report.wButtons & ~bit->second);
            }
            return PushReport(target, report, error);
        }

        if (event.type == "gamepad_axis") {
            std::string control = LowerCopy(event.data.value("control", std::string{}));
            std::replace(control.begin(), control.end(), '-', '_');
            std::replace(control.begin(), control.end(), ' ', '_');
            std::replace(control.begin(), control.end(), '.', '_');

            const std::unordered_map<std::string, std::string> aliases{
                {"lt", "left_trigger"},
                {"rt", "right_trigger"},
                {"l2", "left_trigger"},
                {"r2", "right_trigger"},
                {"left_x", "left_stick_x"},
                {"left_y", "left_stick_y"},
                {"right_x", "right_stick_x"},
                {"right_y", "right_stick_y"},
                {"lx_axis", "left_stick_x"},
                {"ly_axis", "left_stick_y"},
                {"rx_axis", "right_stick_x"},
                {"ry_axis", "right_stick_y"},
                {"left_thumb_x", "left_stick_x"},
                {"left_thumb_y", "left_stick_y"},
                {"right_thumb_x", "right_stick_x"},
                {"right_thumb_y", "right_stick_y"},
                {"left_thumbstick_x", "left_stick_x"},
                {"left_thumbstick_y", "left_stick_y"},
                {"right_thumbstick_x", "right_stick_x"},
                {"right_thumbstick_y", "right_stick_y"},
                {"lstick_x", "left_stick_x"},
                {"lstick_y", "left_stick_y"},
                {"rstick_x", "right_stick_x"},
                {"rstick_y", "right_stick_y"},
                {"leftstick_x", "left_stick_x"},
                {"leftstick_y", "left_stick_y"},
                {"rightstick_x", "right_stick_x"},
                {"rightstick_y", "right_stick_y"},
            };
            if (auto it = aliases.find(control); it != aliases.end()) {
                control = it->second;
            }

            const double value = GetDouble(event.data.value("value", 0), 0);
            if (control == "left_stick_x") {
                report.sThumbLX = ClampStick(value);
            } else if (control == "left_stick_y") {
                report.sThumbLY = ClampStick(value);
            } else if (control == "right_stick_x") {
                report.sThumbRX = ClampStick(value);
            } else if (control == "right_stick_y") {
                report.sThumbRY = ClampStick(value);
            } else if (control == "left_trigger") {
                report.bLeftTrigger = ClampTrigger(value);
            } else if (control == "right_trigger") {
                report.bRightTrigger = ClampTrigger(value);
            } else {
                error = L"Unsupported gamepad axis control: " + WideFromUtf8(control);
                return false;
            }
            return PushReport(target, report, error);
        }

        error = L"Unsupported gamepad event type: " + WideFromUtf8(event.type);
        return false;
    }

private:
    struct Api {
        PVIGEM_CLIENT (*alloc)() = nullptr;
        void (*free)(PVIGEM_CLIENT) = nullptr;
        VIGEM_ERROR (*connect)(PVIGEM_CLIENT) = nullptr;
        void (*disconnect)(PVIGEM_CLIENT) = nullptr;
        PVIGEM_TARGET (*x360_alloc)() = nullptr;
        void (*target_free)(PVIGEM_TARGET) = nullptr;
        VIGEM_ERROR (*target_add)(PVIGEM_CLIENT, PVIGEM_TARGET) = nullptr;
        VIGEM_ERROR (*target_remove)(PVIGEM_CLIENT, PVIGEM_TARGET) = nullptr;
        VIGEM_ERROR (*x360_update)(PVIGEM_CLIENT, PVIGEM_TARGET, XUSB_REPORT) = nullptr;
    } api_{};

    HMODULE dll_ = nullptr;
    PVIGEM_CLIENT client_ = nullptr;
    bool ready_ = false;
    std::map<int, PVIGEM_TARGET> targets_;
    std::map<int, XUSB_REPORT> reports_;

    static bool DriverInstalled() {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\ViGEmBus", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            RegCloseKey(key);
            return true;
        }
        return false;
    }

    static std::optional<fs::path> FindInstaller(std::wstring& error) {
        std::wstring extract_error;
        if (auto extracted = ExtractEmbeddedResource(IDR_VIGEMBUS_SETUP, L"ViGEmBusSetup.exe", extract_error); extracted.has_value()) {
            return extracted;
        }

        wchar_t exe_path[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
            fs::path candidate = fs::path(exe_path).parent_path() / L"vendor" / L"ViGEmBusSetup.exe";
            if (fs::exists(candidate)) {
                return candidate;
            }
        }
        fs::path cwd_candidate = fs::current_path() / L"vendor" / L"ViGEmBusSetup.exe";
        if (fs::exists(cwd_candidate)) {
            return cwd_candidate;
        }
        error = L"ViGEmBus installer is missing. Embedded extract error: " + extract_error;
        return std::nullopt;
    }

    bool InstallDriver(std::wstring& error) {
        if (DriverInstalled()) {
            return true;
        }
        auto installer = FindInstaller(error);
        if (!installer.has_value()) {
            if (error.empty()) {
                error = L"ViGEmBus is not installed and installer was not found.";
            }
            return false;
        }

        wchar_t temp_path[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp_path);
        fs::path log_path = fs::path(temp_path) / L"InputActorCpp-ViGEmBus-install.log";

        std::wstring args = L"/qn /norestart /log \"" + log_path.wstring() + L"\"";

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = installer->c_str();
        sei.lpParameters = args.c_str();
        sei.lpDirectory = installer->parent_path().c_str();
        sei.nShow = SW_HIDE;

        if (!ShellExecuteExW(&sei)) {
            DWORD code = GetLastError();
            if (code == ERROR_CANCELLED) {
                error = L"User canceled the UAC prompt for ViGEmBus installation.";
            } else {
                error = L"Failed to start ViGEmBus installer: " + FormatError(code);
            }
            return false;
        }

        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(sei.hProcess, &exit_code);
        CloseHandle(sei.hProcess);

        if (exit_code != 0 && exit_code != 1641 && exit_code != 3010) {
            error = L"ViGEmBus installer failed with exit code " + std::to_wstring(exit_code) + L". Log: " + log_path.wstring();
            return false;
        }

        for (int i = 0; i < 12; ++i) {
            if (DriverInstalled()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        error = L"ViGEmBus installer finished but driver was not detected. Log: " + log_path.wstring();
        return false;
    }

    bool LoadClientDll(std::wstring& error) {
        if (dll_ != nullptr) {
            return true;
        }

        std::vector<fs::path> candidates;
        std::wstring extract_error;
        if (auto extracted = ExtractEmbeddedResource(IDR_VIGEMCLIENT_DLL, L"ViGEmClient.dll", extract_error); extracted.has_value()) {
            candidates.push_back(*extracted);
        }

        candidates.emplace_back(L"ViGEmClient.dll");

        wchar_t exe_path[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
            fs::path exe_dir = fs::path(exe_path).parent_path();
            candidates.emplace_back(exe_dir / L"ViGEmClient.dll");
            candidates.emplace_back(exe_dir / L"vendor" / L"ViGEmClient.dll");
        }

        const fs::path cwd = fs::current_path();
        candidates.emplace_back(cwd / L"ViGEmClient.dll");
        candidates.emplace_back(cwd / L"vendor" / L"ViGEmClient.dll");
        candidates.emplace_back(cwd / L".venv" / L"Lib" / L"site-packages" / L"vgamepad" / L"win" / L"vigem" / L"client" / L"x64" / L"ViGEmClient.dll");

        for (const auto& candidate : candidates) {
            if (candidate.is_relative()) {
                dll_ = LoadLibraryW(candidate.c_str());
            } else if (fs::exists(candidate)) {
                dll_ = LoadLibraryW(candidate.c_str());
            } else {
                continue;
            }

            if (dll_ != nullptr) {
                return true;
            }
        }

        error = L"Unable to load ViGEmClient.dll. Embedded extract error: " + extract_error;
        return false;
    }

    bool ResolveApi(std::wstring& error) {
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll_, name));
            if (fn == nullptr) {
                error = L"Missing ViGEm API symbol: " + WideFromUtf8(name);
                return false;
            }
            return true;
        };

        return get(api_.alloc, "vigem_alloc") &&
               get(api_.free, "vigem_free") &&
               get(api_.connect, "vigem_connect") &&
               get(api_.disconnect, "vigem_disconnect") &&
               get(api_.x360_alloc, "vigem_target_x360_alloc") &&
               get(api_.target_free, "vigem_target_free") &&
               get(api_.target_add, "vigem_target_add") &&
               get(api_.target_remove, "vigem_target_remove") &&
               get(api_.x360_update, "vigem_target_x360_update");
    }

    bool EnsureTarget(int index, std::wstring& error) {
        for (int i = 0; i <= index; ++i) {
            if (targets_.contains(i)) {
                continue;
            }
            PVIGEM_TARGET target = api_.x360_alloc();
            if (!target) {
                error = L"vigem_target_x360_alloc failed.";
                return false;
            }

            VIGEM_ERROR add_result = api_.target_add(client_, target);
            if (add_result != VIGEM_ERROR_NONE && add_result != VIGEM_ERROR_ALREADY_CONNECTED) {
                api_.target_free(target);
                error = L"vigem_target_add failed with error code " + std::to_wstring(add_result) + L".";
                return false;
            }

            targets_[i] = target;
            reports_[i] = XUSB_REPORT{};
            std::wstring push_error;
            if (!PushReport(target, reports_[i], push_error)) {
                error = push_error;
                return false;
            }
        }
        return true;
    }

    bool PushReport(PVIGEM_TARGET target, const XUSB_REPORT& report, std::wstring& error) {
        VIGEM_ERROR result = api_.x360_update(client_, target, report);
        if (result != VIGEM_ERROR_NONE) {
            error = L"vigem_target_x360_update failed with error code " + std::to_wstring(result) + L".";
            return false;
        }
        return true;
    }

    static uint8_t ClampTrigger(double value) {
        if (value >= 0.0 && value <= 1.0) {
            value *= 255.0;
        } else if (value > 255.0) {
            value = value / 32767.0 * 255.0;
        }
        int v = static_cast<int>(std::round(value));
        return static_cast<uint8_t>(std::clamp(v, 0, 255));
    }

    static int16_t ClampStick(double value) {
        if (value >= -1.0 && value <= 1.0) {
            value *= 32767.0;
        }
        int v = static_cast<int>(std::round(value));
        return static_cast<int16_t>(std::clamp(v, -32768, 32767));
    }
};

class WindowsInputSender {
public:
    void Configure(const SessionMeta& meta) {
        meta_ = meta;
    }

    void SetGamepadMirrorSlot(std::optional<int> slot) {
        gamepad_mirror_slot_ = slot;
        gamepad_.Reset();
    }

    void Prepare(const std::vector<ReplayEvent>& events) {
        for (const auto& event : events) {
            if (event.type.rfind("gamepad_", 0) == 0) {
                std::wstring error;
                if (!gamepad_.EnsureReady(error)) {
                    throw std::runtime_error(Utf8FromWide(error));
                }
                break;
            }
        }
    }

    bool Send(const ReplayEvent& event) {
        if (event.type.rfind("gamepad_", 0) == 0) {
            const ReplayEvent* replay_event = &event;
            ReplayEvent remapped_event{};
            if (gamepad_mirror_slot_.has_value()) {
                remapped_event = event;
                remapped_event.data["gamepad_index"] = *gamepad_mirror_slot_;
                replay_event = &remapped_event;
            }

            std::wstring error;
            if (!gamepad_.Send(*replay_event, error)) {
                throw std::runtime_error(Utf8FromWide(error));
            }
            return true;
        }

        if (event.type == "mouse_move") {
            int dx = static_cast<int>(GetInt64(event.data.value("dx", 0), 0));
            int dy = static_cast<int>(GetInt64(event.data.value("dy", 0), 0));
            return SendMouseMove(dx, dy);
        }

        if (event.type == "key_down" || event.type == "key_up") {
            SendKey(event.data, event.type == "key_up");
            return true;
        }

        if (event.type == "mouse_left_down" || event.type == "mouse_right_down" ||
            event.type == "mouse_middle_down" || event.type == "mouse_x1_down" || event.type == "mouse_x2_down") {
            SendMouseButton(ButtonFromType(event.type), true);
            return true;
        }

        if (event.type == "mouse_left_up" || event.type == "mouse_right_up" ||
            event.type == "mouse_middle_up" || event.type == "mouse_x1_up" || event.type == "mouse_x2_up") {
            SendMouseButton(ButtonFromType(event.type), false);
            return true;
        }

        if (event.type == "mouse_down" || event.type == "mouse_up") {
            std::string button = LowerCopy(event.data.value("button", std::string("left")));
            SendMouseButton(button, event.type == "mouse_down");
            return true;
        }

        if (event.type == "mouse_wheel" || event.type == "mouse_hwheel") {
            int delta = static_cast<int>(GetInt64(event.data.value("delta", event.data.value("wheel_delta", 0)), 0));
            SendMouseWheel(delta, event.type == "mouse_hwheel");
            return true;
        }

        return false;
    }

    void Close() {
        gamepad_.Reset();
    }

private:
    SessionMeta meta_{};
    VigemRuntime gamepad_{};
    std::optional<int> gamepad_mirror_slot_;

    static bool SendInputOnce(INPUT& input) {
        UINT sent = SendInput(1, &input, sizeof(INPUT));
        if (sent != 1) {
            throw std::runtime_error(Utf8FromWide(FormatError(GetLastError())));
        }
        return true;
    }

    static bool SendMouseMove(int dx, int dy) {
        if (dx == 0 && dy == 0) {
            return true;
        }
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        return SendInputOnce(input);
    }

    static void SendKey(const json& data, bool key_up) {
        WORD vk = static_cast<WORD>(GetInt64(data.value("vk", 0), 0));
        WORD scan = static_cast<WORD>(GetInt64(data.value("scan", 0), 0));
        bool extended = data.value("is_extended", false);

        DWORD flags = 0;
        if (key_up) {
            flags |= KEYEVENTF_KEYUP;
        }
        if (extended) {
            flags |= KEYEVENTF_EXTENDEDKEY;
        }
        if (vk == 0 && scan > 0) {
            flags |= KEYEVENTF_SCANCODE;
        }

        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.wScan = scan;
        input.ki.dwFlags = flags;
        SendInputOnce(input);
    }

    static std::string ButtonFromType(const std::string& type) {
        if (type.find("right") != std::string::npos) {
            return "right";
        }
        if (type.find("middle") != std::string::npos) {
            return "middle";
        }
        if (type.find("x1") != std::string::npos) {
            return "x1";
        }
        if (type.find("x2") != std::string::npos) {
            return "x2";
        }
        return "left";
    }

    static void SendMouseButton(const std::string& button, bool down) {
        DWORD flags = 0;
        DWORD data = 0;
        if (button == "left") {
            flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        } else if (button == "right") {
            flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        } else if (button == "middle") {
            flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        } else if (button == "x1") {
            flags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            data = XBUTTON1;
        } else if (button == "x2") {
            flags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            data = XBUTTON2;
        } else {
            return;
        }

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.mouseData = data;
        input.mi.dwFlags = flags;
        SendInputOnce(input);
    }

    static void SendMouseWheel(int delta, bool horizontal) {
        if (delta == 0) {
            return;
        }
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.mouseData = static_cast<DWORD>(delta);
        input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        SendInputOnce(input);
    }
};

class InputPlayer {
public:
    enum class State {
        Idle,
        Ready,
        Running,
        Paused,
        Stopping,
        Finished,
        Stopped,
        Error,
    };

    using StatusCb = std::function<void(const std::wstring&)>;
    using ProgressCb = std::function<void(size_t, size_t, const std::wstring&)>;
    using FinishCb = std::function<void(const std::wstring&)>;

    InputPlayer(StatusCb on_status, ProgressCb on_progress, FinishCb on_finish)
        : on_status_(std::move(on_status)),
          on_progress_(std::move(on_progress)),
          on_finish_(std::move(on_finish)) {}

    ~InputPlayer() {
        Stop();
        if (worker_.joinable()) {
            worker_.join();
        }
        sender_.Close();
    }

    void Load(const fs::path& path) {
        {
            std::unique_lock<std::mutex> guard(lock_);
            if (state_ == State::Running || state_ == State::Paused || state_ == State::Stopping) {
                throw std::runtime_error("Cannot load while playback is running.");
            }

            if (worker_.joinable()) {
                guard.unlock();
                worker_.join();
                guard.lock();
            }
        }

        SessionMeta loaded_meta{};
        std::vector<ReplayEvent> loaded_events;
        std::wstring error;
        if (!LoadJsonl(path, loaded_meta, loaded_events, error)) {
            throw std::runtime_error(Utf8FromWide(error));
        }

        sender_.Configure(loaded_meta);
        {
            std::lock_guard<std::mutex> guard(lock_);
            meta_ = loaded_meta;
            events_ = std::move(loaded_events);
            state_ = State::Ready;
            current_index_ = 0;
            path_ = path;
        }
        on_status_(L"Loaded " + std::to_wstring(TotalEvents()) + L" events from " + path.wstring());
        on_progress_(0, TotalEvents(), L"-");
    }

    bool Start() {
        std::vector<ReplayEvent> snapshot;
        {
            std::unique_lock<std::mutex> guard(lock_);
            if (events_.empty()) {
                throw std::runtime_error("No input sequence loaded.");
            }

            if (worker_.joinable()) {
                if (state_ == State::Running || state_ == State::Paused || state_ == State::Stopping) {
                    return false;
                }

                guard.unlock();
                worker_.join();
                guard.lock();
            }
            snapshot = events_;
        }

        sender_.Prepare(snapshot);

        {
            std::lock_guard<std::mutex> guard(lock_);
            stop_requested_ = false;
            paused_ = false;
            current_index_ = 0;
            state_ = State::Running;
        }
        worker_ = std::thread([this]() { Run(); });
        on_status_(L"Playback started.");
        return true;
    }

    void SetGamepadMirrorSlot(std::optional<int> slot) {
        std::lock_guard<std::mutex> guard(lock_);
        if (state_ == State::Running || state_ == State::Paused || state_ == State::Stopping) {
            throw std::runtime_error("Cannot change gamepad slot while playback is running.");
        }
        sender_.SetGamepadMirrorSlot(slot);
    }

    bool TogglePause() {
        std::lock_guard<std::mutex> guard(lock_);
        if (state_ == State::Running) {
            paused_ = true;
            state_ = State::Paused;
            on_status_(L"Playback paused.");
            return true;
        }
        if (state_ == State::Paused) {
            paused_ = false;
            state_ = State::Running;
            pause_cv_.notify_all();
            on_status_(L"Playback resumed.");
            return true;
        }
        return false;
    }

    bool Stop() {
        std::lock_guard<std::mutex> guard(lock_);
        if (!(state_ == State::Running || state_ == State::Paused)) {
            return false;
        }
        stop_requested_ = true;
        paused_ = false;
        state_ = State::Stopping;
        pause_cv_.notify_all();
        on_status_(L"Stopping playback...");
        return true;
    }

    State GetState() const {
        std::lock_guard<std::mutex> guard(lock_);
        return state_;
    }

    size_t TotalEvents() const {
        std::lock_guard<std::mutex> guard(lock_);
        return events_.size();
    }

private:
    mutable std::mutex lock_;
    std::condition_variable pause_cv_;
    WindowsInputSender sender_;
    SessionMeta meta_{};
    std::vector<ReplayEvent> events_;
    fs::path path_;
    std::thread worker_;
    bool stop_requested_ = false;
    bool paused_ = false;
    State state_ = State::Idle;
    size_t current_index_ = 0;

    StatusCb on_status_;
    ProgressCb on_progress_;
    FinishCb on_finish_;

    bool WaitUntil(SteadyClock::time_point& started_wall, SteadyClock::time_point& target_wall) {
        std::optional<SteadyClock::time_point> paused_since;

        while (true) {
            {
                std::unique_lock<std::mutex> guard(lock_);
                if (stop_requested_) {
                    return false;
                }
                if (paused_) {
                    if (!paused_since.has_value()) {
                        paused_since = SteadyClock::now();
                    }
                    pause_cv_.wait_for(guard, std::chrono::milliseconds(50), [this]() { return !paused_ || stop_requested_; });
                    continue;
                }
            }

            if (paused_since.has_value()) {
                auto now = SteadyClock::now();
                auto paused_duration = now - *paused_since;
                started_wall += paused_duration;
                target_wall += paused_duration;
                paused_since.reset();
            }

            auto now = SteadyClock::now();
            if (now >= target_wall) {
                return true;
            }

            auto remaining = target_wall - now;
            const auto cap = std::chrono::duration_cast<SteadyClock::duration>(std::chrono::milliseconds(5));
            std::this_thread::sleep_for((remaining < cap) ? remaining : cap);
        }
    }

    void Run() {
        std::wstring reason = L"completed";

        std::vector<ReplayEvent> snapshot;
        SessionMeta meta{};
        {
            std::lock_guard<std::mutex> guard(lock_);
            snapshot = events_;
            meta = meta_;
        }

        if (snapshot.empty()) {
            reason = L"error";
            on_status_(L"Playback failed: no events loaded.");
            Finish(reason);
            return;
        }

        int64_t first_t = snapshot.front().t_qpc;
        double qpc_freq = static_cast<double>(std::max<int64_t>(1, meta.qpc_freq));
        auto started_wall = SteadyClock::now();

        for (size_t i = 0; i < snapshot.size(); ++i) {
            {
                std::lock_guard<std::mutex> guard(lock_);
                if (stop_requested_) {
                    reason = L"stopped";
                    break;
                }
            }

            const auto& event = snapshot[i];
            double offset = static_cast<double>(event.t_qpc - first_t) / qpc_freq;
            auto target_wall = started_wall + std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(offset));

            if (!WaitUntil(started_wall, target_wall)) {
                reason = L"stopped";
                break;
            }

            try {
                sender_.Send(event);
            } catch (const std::exception& ex) {
                reason = L"error";
                on_status_(L"Playback failed: " + WideFromUtf8(ex.what()));
                break;
            }

            {
                std::lock_guard<std::mutex> guard(lock_);
                current_index_ = i + 1;
            }
            on_progress_(i + 1, snapshot.size(), WideFromUtf8(event.type));
        }

        Finish(reason);
    }

    void Finish(const std::wstring& reason) {
        sender_.Close();

        {
            std::lock_guard<std::mutex> guard(lock_);
            paused_ = false;
            if (reason == L"completed") {
                state_ = State::Finished;
            } else if (reason == L"stopped") {
                state_ = State::Stopped;
            } else {
                state_ = State::Error;
            }
        }

        if (reason == L"completed") {
            on_status_(L"Playback finished.");
        } else if (reason == L"stopped") {
            on_status_(L"Playback stopped.");
        }
        on_finish_(reason);
    }
};

std::wstring StateName(InputPlayer::State state) {
    switch (state) {
        case InputPlayer::State::Idle:
            return L"idle";
        case InputPlayer::State::Ready:
            return L"ready";
        case InputPlayer::State::Running:
            return L"running";
        case InputPlayer::State::Paused:
            return L"paused";
        case InputPlayer::State::Stopping:
            return L"stopping";
        case InputPlayer::State::Finished:
            return L"finished";
        case InputPlayer::State::Stopped:
            return L"stopped";
        case InputPlayer::State::Error:
            return L"error";
        default:
            return L"unknown";
    }
}

struct ProgressPayload {
    size_t current = 0;
    size_t total = 0;
    std::wstring event_type;
};

class AppWindow {
public:
    AppWindow(HINSTANCE instance)
        : instance_(instance),
          player_(
              [this](const std::wstring& msg) { PostStatus(msg); },
              [this](size_t current, size_t total, const std::wstring& evt) { PostProgress(current, total, evt); },
              [this](const std::wstring& reason) { PostFinish(reason); }) {}

    int Run(int nCmdShow) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &AppWindow::WndProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"InputActorCppWindow";
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

        if (!RegisterClassExW(&wc)) {
            return 1;
        }

        hwnd_ = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"Input Actor C++",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            900,
            640,
            nullptr,
            nullptr,
            instance_,
            this);

        if (!hwnd_) {
            return 1;
        }

        ShowWindow(hwnd_, nCmdShow);
        UpdateWindow(hwnd_);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        return static_cast<int>(msg.wParam);
    }

private:
    HINSTANCE instance_{};
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HWND path_edit_ = nullptr;
    HWND start_button_ = nullptr;
    HWND pause_button_ = nullptr;
    HWND stop_button_ = nullptr;
    HWND delay_edit_ = nullptr;
    HWND progress_bar_ = nullptr;
    HWND events_static_ = nullptr;
    HWND event_type_static_ = nullptr;
    HWND status_static_ = nullptr;
    HWND state_static_ = nullptr;
    HWND gamepad_slot_combo_ = nullptr;
    HWND log_edit_ = nullptr;
    std::wstring loaded_path_key_;

    InputPlayer player_;
    bool countdown_active_ = false;
    SteadyClock::time_point countdown_end_{};
    int countdown_last_sec_ = -1;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        AppWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<AppWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self) {
            return self->HandleMessage(msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE:
                BuildUi();
                RegisterHotkeys();
                LoadDefaultInput();
                RefreshButtons();
                return 0;
            case WM_COMMAND:
                HandleCommand(LOWORD(wp), HIWORD(wp));
                return 0;
            case WM_HOTKEY:
                if (wp == HOTKEY_ID_START) {
                    StartPlayback();
                } else if (wp == HOTKEY_ID_PAUSE) {
                    TogglePause();
                } else if (wp == HOTKEY_ID_STOP) {
                    StopPlayback();
                }
                return 0;
            case WM_TIMER:
                if (wp == TIMER_COUNTDOWN) {
                    TickCountdown();
                }
                return 0;
            case WM_APP_STATUS:
                OnStatusMessage(reinterpret_cast<std::wstring*>(lp));
                return 0;
            case WM_APP_PROGRESS:
                OnProgressMessage(reinterpret_cast<ProgressPayload*>(lp));
                return 0;
            case WM_APP_FINISH:
                OnFinishMessage(reinterpret_cast<std::wstring*>(lp));
                return 0;
            case WM_CLOSE:
                Shutdown();
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hwnd_, msg, wp, lp);
        }
    }

    void BuildUi() {
        font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        CreateStatic(L"Input Sequence", 16, 12, 200, 20);
        path_edit_ = CreateCtrl(L"EDIT", L"", WS_BORDER | WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 36, 620, 24, IDC_PATH);
        CreateCtrl(L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE, 646, 36, 100, 24, IDC_BROWSE);
        CreateCtrl(L"BUTTON", L"Load", WS_CHILD | WS_VISIBLE, 756, 36, 100, 24, IDC_LOAD);

        start_button_ = CreateCtrl(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE, 16, 76, 80, 28, IDC_START);
        pause_button_ = CreateCtrl(L"BUTTON", L"Pause", WS_CHILD | WS_VISIBLE, 104, 76, 80, 28, IDC_PAUSE);
        stop_button_ = CreateCtrl(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE, 192, 76, 80, 28, IDC_STOP);

        CreateStatic(L"Start Delay(s):", 290, 82, 100, 20);
        delay_edit_ = CreateCtrl(L"EDIT", L"3", WS_BORDER | WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 388, 78, 64, 24, IDC_DELAY);
        CreateStatic(L"State:", 470, 82, 44, 20);
        state_static_ = CreateCtrl(L"STATIC", L"idle", WS_CHILD | WS_VISIBLE, 514, 82, 160, 20, IDC_STATE);
        CreateStatic(L"Gamepad Slot:", 680, 82, 90, 20);
        gamepad_slot_combo_ = CreateCtrl(WC_COMBOBOXW, L"", WS_BORDER | WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                         774, 78, 82, 200, IDC_GAMEPAD_SLOT);
        SendMessageW(gamepad_slot_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto"));
        SendMessageW(gamepad_slot_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0"));
        SendMessageW(gamepad_slot_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1"));
        SendMessageW(gamepad_slot_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2"));
        SendMessageW(gamepad_slot_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"3"));
        SendMessageW(gamepad_slot_combo_, CB_SETCURSEL, 0, 0);
        ApplyGamepadSlotSelection(false);

        CreateStatic(L"Hotkeys", 16, 114, 200, 20);
        CreateStatic(DEFAULT_START_HOTKEY, 16, 136, 180, 20);
        CreateStatic(DEFAULT_PAUSE_HOTKEY, 210, 136, 180, 20);
        CreateStatic(DEFAULT_STOP_HOTKEY, 404, 136, 180, 20);

        CreateStatic(L"Playback Progress", 16, 168, 200, 20);
        progress_bar_ = CreateCtrl(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 16, 192, 840, 22, IDC_PROGRESS);
        SendMessageW(progress_bar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        CreateStatic(L"Events:", 16, 222, 56, 20);
        events_static_ = CreateCtrl(L"STATIC", L"0 / 0", WS_CHILD | WS_VISIBLE, 72, 222, 120, 20, IDC_EVENTS);
        CreateStatic(L"Current Event:", 208, 222, 100, 20);
        event_type_static_ = CreateCtrl(L"STATIC", L"-", WS_CHILD | WS_VISIBLE, 312, 222, 240, 20, IDC_EVENT_TYPE);
        status_static_ = CreateCtrl(L"STATIC", L"Ready.", WS_CHILD | WS_VISIBLE, 16, 246, 840, 20, IDC_STATUS);

        CreateStatic(L"Runtime Log", 16, 274, 200, 20);
        log_edit_ = CreateCtrl(L"EDIT", L"", WS_BORDER | WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                               16, 298, 840, 292, IDC_LOG);
    }

    HWND CreateCtrl(const wchar_t* klass, const wchar_t* text, DWORD style, int x, int y, int w, int hgt, int id) {
        HWND hwnd_ctrl = CreateWindowExW(0, klass, text, style, x, y, w, hgt, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(hwnd_ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return hwnd_ctrl;
    }

    void CreateStatic(const wchar_t* text, int x, int y, int w, int hgt) {
        HWND hwnd_ctrl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, hgt, hwnd_, nullptr, instance_, nullptr);
        SendMessageW(hwnd_ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    void HandleCommand(int id, int notify_code) {
        if (id == IDC_BROWSE) {
            BrowseFile();
        } else if (id == IDC_LOAD) {
            LoadSequence(true);
        } else if (id == IDC_START) {
            StartPlayback();
        } else if (id == IDC_PAUSE) {
            TogglePause();
        } else if (id == IDC_STOP) {
            StopPlayback();
        } else if (id == IDC_GAMEPAD_SLOT && notify_code == CBN_SELCHANGE) {
            ApplyGamepadSlotSelection(true);
        }
    }

    std::wstring GetText(HWND control) const {
        int len = GetWindowTextLengthW(control);
        std::wstring text(static_cast<size_t>(std::max(len, 0)) + 1, L'\0');
        if (len > 0) {
            GetWindowTextW(control, text.data(), len + 1);
        }
        if (!text.empty()) {
            text.resize(static_cast<size_t>(len));
        }
        return text;
    }

    void SetText(HWND control, const std::wstring& text) {
        SetWindowTextW(control, text.c_str());
    }

    static std::wstring NormalizePathKey(const std::wstring& path_text) {
        if (path_text.empty()) {
            return L"";
        }
        fs::path path(path_text);
        std::error_code ec;
        fs::path absolute = fs::absolute(path, ec);
        if (ec) {
            absolute = path;
        }
        fs::path normalized = absolute.lexically_normal();
        std::wstring key = LowerCopy(normalized.wstring());
        std::replace(key.begin(), key.end(), L'/', L'\\');
        return key;
    }

    void SetStatus(const std::wstring& text) {
        SetText(status_static_, text);
        AppendLog(text);
    }

    void AppendLog(const std::wstring& text) {
        std::wstring line = TimeStamp() + text + L"\r\n";
        int len = GetWindowTextLengthW(log_edit_);
        SendMessageW(log_edit_, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
        SendMessageW(log_edit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    }

    void UpdateProgress(size_t current, size_t total, const std::wstring& event_type) {
        if (total == 0) {
            total = 1;
        }
        int percent = static_cast<int>(std::clamp((100.0 * static_cast<double>(current)) / static_cast<double>(total), 0.0, 100.0));
        SendMessageW(progress_bar_, PBM_SETPOS, percent, 0);
        SetText(events_static_, std::to_wstring(current) + L" / " + std::to_wstring(total));
        SetText(event_type_static_, event_type);
    }

    bool LoadSequence(bool show_dialog) {
        std::wstring path = GetText(path_edit_);
        if (path.empty()) {
            if (show_dialog) {
                MessageBoxW(hwnd_, L"Please choose a .jsonl file first.", L"Load failed", MB_ICONERROR | MB_OK);
            }
            return false;
        }

        try {
            player_.Load(path);
            loaded_path_key_ = NormalizePathKey(path);
            SetStatus(L"Loaded: " + path);
            RefreshButtons();
            return true;
        } catch (const std::exception& ex) {
            std::wstring msg = WideFromUtf8(ex.what());
            SetStatus(L"Load failed: " + msg);
            if (show_dialog) {
                MessageBoxW(hwnd_, msg.c_str(), L"Load failed", MB_ICONERROR | MB_OK);
            }
            RefreshButtons();
            return false;
        }
    }

    void StartPlayback() {
        if (countdown_active_) {
            SetStatus(L"Countdown is already running.");
            return;
        }

        auto state = player_.GetState();
        if (state == InputPlayer::State::Paused) {
            SetStatus(L"Playback is paused. Use Pause to resume.");
            return;
        }
        if (state == InputPlayer::State::Running || state == InputPlayer::State::Stopping) {
            SetStatus(L"Playback is already running.");
            return;
        }

        if (!ApplyGamepadSlotSelection(false)) {
            return;
        }

        const std::wstring path = GetText(path_edit_);
        const std::wstring path_key = NormalizePathKey(path);
        const bool need_reload = player_.TotalEvents() == 0 || path_key.empty() || path_key != loaded_path_key_;
        if (need_reload && !LoadSequence(true)) {
            return;
        }

        wchar_t* end_ptr = nullptr;
        std::wstring delay_str = GetText(delay_edit_);
        if (delay_str.empty()) {
            delay_str = L"3";
            SetText(delay_edit_, delay_str);
        }
        double delay = std::wcstod(delay_str.c_str(), &end_ptr);
        if (end_ptr == delay_str.c_str() || delay < 0.0) {
            MessageBoxW(hwnd_, L"Start delay must be a non-negative number.", L"Invalid delay", MB_ICONERROR | MB_OK);
            return;
        }

        if (delay <= 0.0) {
            StartNow();
            return;
        }

        countdown_active_ = true;
        countdown_last_sec_ = -1;
        countdown_end_ = SteadyClock::now() + std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(delay));
        SetStatus(L"Countdown started: playback will begin in " + std::to_wstring(delay) + L"s.");
        SetTimer(hwnd_, TIMER_COUNTDOWN, 100, nullptr);
        RefreshButtons();
    }

    void StartNow() {
        try {
            if (!player_.Start()) {
                SetStatus(L"Playback is already running.");
            }
        } catch (const std::exception& ex) {
            std::wstring msg = WideFromUtf8(ex.what());
            SetStatus(L"Cannot start playback: " + msg);
            MessageBoxW(hwnd_, msg.c_str(), L"Start failed", MB_ICONERROR | MB_OK);
        }
        RefreshButtons();
    }

    void TogglePause() {
        if (countdown_active_) {
            SetStatus(L"Cannot pause during countdown.");
            return;
        }
        if (!player_.TogglePause()) {
            SetStatus(L"Pause/Resume is available only while running.");
        }
        RefreshButtons();
    }

    void StopPlayback() {
        if (countdown_active_) {
            CancelCountdown(L"Countdown canceled.");
            return;
        }
        if (player_.Stop()) {
            SetStatus(L"Stopping requested.");
        } else {
            SetStatus(L"Nothing is running.");
        }
        RefreshButtons();
    }

    void TickCountdown() {
        if (!countdown_active_) {
            KillTimer(hwnd_, TIMER_COUNTDOWN);
            return;
        }

        auto now = SteadyClock::now();
        if (now >= countdown_end_) {
            countdown_active_ = false;
            KillTimer(hwnd_, TIMER_COUNTDOWN);
            SetStatus(L"Countdown finished. Starting playback...");
            StartNow();
            return;
        }

        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(countdown_end_ - now).count();
        int sec = static_cast<int>((remaining_ms + 999) / 1000);
        if (sec != countdown_last_sec_) {
            countdown_last_sec_ = sec;
            SetText(status_static_, L"Starting in " + std::to_wstring(sec) + L"s...");
        }
        RefreshButtons();
    }

    void CancelCountdown(const std::wstring& status) {
        countdown_active_ = false;
        countdown_last_sec_ = -1;
        KillTimer(hwnd_, TIMER_COUNTDOWN);
        if (!status.empty()) {
            SetStatus(status);
        }
        RefreshButtons();
    }

    void RefreshButtons() {
        if (countdown_active_) {
            EnableWindow(start_button_, FALSE);
            EnableWindow(pause_button_, FALSE);
            EnableWindow(stop_button_, TRUE);
            EnableWindow(gamepad_slot_combo_, FALSE);
            SetText(state_static_, L"countdown");
            SetText(pause_button_, L"Pause");
            return;
        }

        auto state = player_.GetState();
        SetText(state_static_, StateName(state));

        bool can_start = state == InputPlayer::State::Idle || state == InputPlayer::State::Ready ||
                         state == InputPlayer::State::Finished || state == InputPlayer::State::Stopped ||
                         state == InputPlayer::State::Error;
        EnableWindow(start_button_, can_start ? TRUE : FALSE);

        bool can_pause_stop = state == InputPlayer::State::Running || state == InputPlayer::State::Paused;
        EnableWindow(pause_button_, can_pause_stop ? TRUE : FALSE);
        EnableWindow(stop_button_, can_pause_stop ? TRUE : FALSE);
        EnableWindow(gamepad_slot_combo_, can_start ? TRUE : FALSE);
        SetText(pause_button_, state == InputPlayer::State::Paused ? L"Resume" : L"Pause");
    }

    static std::optional<int> SlotFromComboSelection(int selection) {
        if (selection <= 0) {
            return std::nullopt;
        }
        return selection - 1;
    }

    bool ApplyGamepadSlotSelection(bool show_status) {
        if (!gamepad_slot_combo_) {
            return true;
        }
        int selection = static_cast<int>(SendMessageW(gamepad_slot_combo_, CB_GETCURSEL, 0, 0));
        if (selection == CB_ERR) {
            selection = 0;
            SendMessageW(gamepad_slot_combo_, CB_SETCURSEL, selection, 0);
        }
        std::optional<int> forced_slot = SlotFromComboSelection(selection);
        try {
            player_.SetGamepadMirrorSlot(forced_slot);
        } catch (const std::exception& ex) {
            std::wstring msg = WideFromUtf8(ex.what());
            SetStatus(L"Cannot apply gamepad slot: " + msg);
            MessageBoxW(hwnd_, msg.c_str(), L"Gamepad slot", MB_ICONERROR | MB_OK);
            return false;
        }

        if (show_status) {
            if (forced_slot.has_value()) {
                SetStatus(L"Gamepad slot forced to XInput #" + std::to_wstring(*forced_slot) + L".");
            } else {
                SetStatus(L"Gamepad slot set to Auto (use gamepad_index from jsonl).");
            }
        }
        return true;
    }

    void RegisterHotkeys() {
        RegisterHotKey(hwnd_, HOTKEY_ID_START, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F6);
        RegisterHotKey(hwnd_, HOTKEY_ID_PAUSE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F7);
        RegisterHotKey(hwnd_, HOTKEY_ID_STOP, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F8);
        SetStatus(L"Hotkeys registered: " + std::wstring(DEFAULT_START_HOTKEY) + L", " +
                  std::wstring(DEFAULT_PAUSE_HOTKEY) + L", " + std::wstring(DEFAULT_STOP_HOTKEY));
    }

    void BrowseFile() {
        wchar_t path_buf[MAX_PATH]{};
        std::wstring current = GetText(path_edit_);
        if (!current.empty()) {
            StringCchCopyW(path_buf, MAX_PATH, current.c_str());
        }

        wchar_t filter[] = L"JSONL files (*.jsonl)\0*.jsonl\0All files (*.*)\0*.*\0\0";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = path_buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        ofn.lpstrTitle = L"Choose input sequence";

        if (GetOpenFileNameW(&ofn)) {
            SetText(path_edit_, path_buf);
        }
    }

    void LoadDefaultInput() {
        fs::path candidate = fs::current_path() / L"example" / L"input.jsonl";
        if (!fs::exists(candidate)) {
            for (const auto& entry : fs::recursive_directory_iterator(fs::current_path())) {
                if (entry.is_regular_file() && LowerCopy(entry.path().extension().wstring()) == L".jsonl") {
                    candidate = entry.path();
                    break;
                }
            }
        }

        if (fs::exists(candidate)) {
            SetText(path_edit_, candidate.wstring());
            LoadSequence(false);
        }
    }

    void PostStatus(const std::wstring& message) {
        if (!hwnd_) {
            return;
        }
        auto* payload = new std::wstring(message);
        PostMessageW(hwnd_, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(payload));
    }

    void PostProgress(size_t current, size_t total, const std::wstring& event_type) {
        if (!hwnd_) {
            return;
        }
        auto* payload = new ProgressPayload{current, total, event_type};
        PostMessageW(hwnd_, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(payload));
    }

    void PostFinish(const std::wstring& reason) {
        if (!hwnd_) {
            return;
        }
        auto* payload = new std::wstring(reason);
        PostMessageW(hwnd_, WM_APP_FINISH, 0, reinterpret_cast<LPARAM>(payload));
    }

    void OnStatusMessage(std::wstring* payload) {
        if (!payload) {
            return;
        }
        SetStatus(*payload);
        delete payload;
        RefreshButtons();
    }

    void OnProgressMessage(ProgressPayload* payload) {
        if (!payload) {
            return;
        }
        UpdateProgress(payload->current, payload->total, payload->event_type);
        delete payload;
        RefreshButtons();
    }

    void OnFinishMessage(std::wstring* payload) {
        delete payload;
        RefreshButtons();
    }

    void Shutdown() {
        CancelCountdown(L"");
        player_.Stop();
        UnregisterHotKey(hwnd_, HOTKEY_ID_START);
        UnregisterHotKey(hwnd_, HOTKEY_ID_PAUSE);
        UnregisterHotKey(hwnd_, HOTKEY_ID_STOP);
        DestroyWindow(hwnd_);
    }
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    AppWindow app(instance);
    return app.Run(nCmdShow);
}
