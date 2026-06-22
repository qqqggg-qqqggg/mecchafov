#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <chrono>
#include <conio.h>
#include <atomic>

// ── Offsets
constexpr uintptr_t GWorld = 0xA0AF170;

constexpr uintptr_t UWorld_GameInstance        = 0x228;
constexpr uintptr_t GI_LocalPlayers            = 0x38;
constexpr uintptr_t ULocalPlayer_Controller    = 0x30;
constexpr uintptr_t AController_CameraManager  = 0x360;

constexpr uintptr_t CameraManager_CameraCache  = 0x1530;
constexpr uintptr_t CameraManager_LastFrame    = 0x1E00;   
constexpr uintptr_t CameraCache_POV            = 0x10;
constexpr uintptr_t POV_FOV                    = 0x30;

constexpr uintptr_t DefaultFOV    = 0x2C0;
constexpr uintptr_t OrthoWidth    = 0x2C8;
constexpr uintptr_t AspectRatio   = 0x2D0;

constexpr uintptr_t AController_Pawn                     = 0x2E8;
constexpr uintptr_t Pawn_FirstPersonCamera                = 0x428;   // UCameraComponent*
constexpr uintptr_t Pawn_BPC_CameraMoveAnimation          = 0x450;   // UBPC_CameraMoveAnimation_C*
constexpr uintptr_t CameraComponent_FieldOfView           = 0x240;
constexpr uintptr_t CameraComponent_FirstPersonFieldOfView = 0x244;
constexpr uintptr_t BPC_CameraMoveAnimation_DefaultFOV    = 0x118;

HANDLE g_hProcess = NULL;
uintptr_t g_moduleBase = 0;
uintptr_t g_moduleSize = 0;
DWORD g_pid = 0;

std::atomic<float> g_valPOV{90.0f};
std::atomic<float> g_valDef{90.0f};
std::atomic<float> g_valOrtho{512.0f};
std::atomic<float> g_valAspect{1.3333f};
std::atomic<bool> g_running{true};


template<typename T>
T Read(uintptr_t addr) {
    T val = {};
    ReadProcessMemory(g_hProcess, (LPCVOID)addr, &val, sizeof(T), NULL);
    return val;
}

template<typename T>
bool Write(uintptr_t addr, T val) {
    SIZE_T written = 0;
    return WriteProcessMemory(g_hProcess, (LPVOID)addr, &val, sizeof(T), &written) && written == sizeof(T);
}


bool FindProcess() {
    const wchar_t* titles[] = { L"Meccha Chameleon", L"PenguinHotel", L"PenguinHotel-Win64-Shipping" };
    for (auto t : titles) {
        HWND hwnd = FindWindowW(NULL, t);
        if (!hwnd) hwnd = FindWindowW(L"Chameleon", t);
        if (hwnd) {
            GetWindowThreadProcessId(hwnd, &g_pid);
            if (g_pid) {
                g_hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, g_pid);
                if (g_hProcess) return true;
            }
        }
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"PenguinHotel-Win64-Shipping.exe") == 0) {
                g_pid = pe.th32ProcessID;
                g_hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, g_pid);
                found = (g_hProcess != NULL);
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool GetModuleBase() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, g_pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = { sizeof(me) };
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"PenguinHotel-Win64-Shipping.exe") == 0) {
                g_moduleBase = (uintptr_t)me.modBaseAddr;
                g_moduleSize = me.modBaseSize;
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}


struct CameraChain {
    uintptr_t uworld;
    uintptr_t gameInstance;
    uintptr_t localPlayersPtr;
    uintptr_t localPlayer;
    uintptr_t playerController;
    uintptr_t cameraManager;
    uintptr_t pawn;
    uintptr_t cameraComponent;
    uintptr_t cameraAnim;
    uintptr_t pov;
};

CameraChain ResolveChain() {
    CameraChain c = {};
    c.uworld = Read<uintptr_t>(g_moduleBase + GWorld);
    if (!c.uworld) return c;
    c.gameInstance = Read<uintptr_t>(c.uworld + UWorld_GameInstance);
    if (!c.gameInstance) return c;
    c.localPlayersPtr = Read<uintptr_t>(c.gameInstance + GI_LocalPlayers);
    if (!c.localPlayersPtr) return c;
    c.localPlayer = Read<uintptr_t>(c.localPlayersPtr);
    if (!c.localPlayer) return c;
    c.playerController = Read<uintptr_t>(c.localPlayer + ULocalPlayer_Controller);
    if (!c.playerController) return c;
    c.cameraManager = Read<uintptr_t>(c.playerController + AController_CameraManager);
    if (!c.cameraManager) return c;
    c.pov = c.cameraManager + CameraManager_CameraCache + CameraCache_POV;

    c.pawn = Read<uintptr_t>(c.playerController + AController_Pawn);
    if (c.pawn) {
        c.cameraComponent = Read<uintptr_t>(c.pawn + Pawn_FirstPersonCamera);
        c.cameraAnim = Read<uintptr_t>(c.pawn + Pawn_BPC_CameraMoveAnimation);
    }
    return c;
}


void ForceThread() {
    CameraChain chain;
    uint32_t resolveTick = 0;
    while (g_running) {
        if (++resolveTick >= 256) {
            chain = ResolveChain();
            resolveTick = 0;
        }

        float pov = g_valPOV.load();
        float def = g_valDef.load();
        float ortho = g_valOrtho.load();
        float asp = g_valAspect.load();

        if (chain.cameraManager) {
            // CameraCache (current frame's POV)
            Write(chain.cameraManager + CameraManager_CameraCache + CameraCache_POV + POV_FOV, pov);
            // LastFrameCameraCache (renderer may use this)
            Write(chain.cameraManager + CameraManager_LastFrame + CameraCache_POV + POV_FOV, pov);
            // DefaultFOV (persistent base)
            Write(chain.cameraManager + DefaultFOV, def);
            Write(chain.cameraManager + OrthoWidth, ortho);
            Write(chain.cameraManager + AspectRatio, asp);
        }

        if (chain.cameraComponent) {
            Write(chain.cameraComponent + CameraComponent_FieldOfView, def);
            Write(chain.cameraComponent + CameraComponent_FirstPersonFieldOfView, def);
        }

        if (chain.cameraAnim) {
            Write(chain.cameraAnim + BPC_CameraMoveAnimation_DefaultFOV, def);
        }
    }
}

std::string ReadFloatStr() {
    std::string input;
    while (true) {
        int k = _getch();
        if (k == '\r' || k == '\n') break;
        if (k == '\b' || k == 127) {
            if (!input.empty()) { input.pop_back(); std::cout << "\b \b"; }
            continue;
        }
        if ((k >= '0' && k <= '9') || k == '-' || k == '.') {
            input += (char)k;
            std::cout << (char)k;
        }
    }
    std::cout << "\n";
    return input;
}

float ReadLiveFov() {
    CameraChain c;
    c.uworld = Read<uintptr_t>(g_moduleBase + GWorld);
    if (!c.uworld) return 0;
    c.gameInstance = Read<uintptr_t>(c.uworld + UWorld_GameInstance);
    if (!c.gameInstance) return 0;
    c.localPlayersPtr = Read<uintptr_t>(c.gameInstance + GI_LocalPlayers);
    if (!c.localPlayersPtr) return 0;
    c.localPlayer = Read<uintptr_t>(c.localPlayersPtr);
    if (!c.localPlayer) return 0;
    c.playerController = Read<uintptr_t>(c.localPlayer + ULocalPlayer_Controller);
    if (!c.playerController) return 0;
    c.pawn = Read<uintptr_t>(c.playerController + AController_Pawn);
    if (c.pawn) {
        c.cameraComponent = Read<uintptr_t>(c.pawn + Pawn_FirstPersonCamera);
        if (c.cameraComponent)
            return Read<float>(c.cameraComponent + CameraComponent_FieldOfView);
    }
    c.cameraManager = Read<uintptr_t>(c.playerController + AController_CameraManager);
    if (c.cameraManager) return Read<float>(c.cameraManager + DefaultFOV);
    return 0;
}

// ── Main ──

int main() {
    if (!FindProcess()) return 1;
    if (!GetModuleBase()) { CloseHandle(g_hProcess); return 1; }
    if (GWorld >= g_moduleSize) { CloseHandle(g_hProcess); return 1; }

    float initFov = ReadLiveFov();
    if (initFov > 0) { g_valPOV = initFov; g_valDef = initFov; }

    std::thread forceThr(ForceThread);

    system("cls");
    std::cout << "current fov: " << std::fixed << std::setprecision(2) << initFov << "\n";
    std::cout << "\n[1] set fov  [2] reset  [Q] quit\n";

    bool running = true;
    while (running) {
        if (_kbhit()) {
            int ch = _getch();
            switch (ch) {
            case '1': {
                std::cout << "new fov: ";
                std::string inp = ReadFloatStr();
                try { float v = std::stof(inp); if (v > 140.0f) v = 140.0f; if (v < 1.0f) v = 1.0f; g_valPOV = v; g_valDef = v; } catch(...) {}
                std::cout << "current fov: " << std::fixed << std::setprecision(2) << ReadLiveFov() << "\n";
                std::cout << "[1] set fov  [2] reset  [Q] quit\n";
                break;
            }
            case '2':
                g_valPOV = 90.0f; g_valDef = 90.0f;
                std::cout << "current fov: " << std::fixed << std::setprecision(2) << ReadLiveFov() << "\n";
                std::cout << "[1] set fov  [2] reset  [Q] quit\n";
                break;
            case 'q': case 'Q':
                running = false; break;
            }
        }
        Sleep(250);
    }

    g_running = false;
    if (forceThr.joinable()) forceThr.join();
    CloseHandle(g_hProcess);
    return 0;
}
