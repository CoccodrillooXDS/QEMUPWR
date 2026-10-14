#ifndef QEMUPWR_CORE_H
#define QEMUPWR_CORE_H
#include <windows.h>

typedef struct _QEMU_STATE {
    BOOL Installed;
    BOOL Running;
    BOOL Automatic;
    BOOL AgentInstalled;
    BOOL AgentRunning;
    BOOL DriverResponding;
    BOOL HookActive;
    BOOL Compatible;
    BOOL PvpanicAvailable;
    DWORD PvpanicMask;
    DWORD BranchAddress;
} QEMU_STATE;

typedef struct _QEMU_PROBE {
    BOOL Available;
    BOOL Compatible;
    BOOL TestedKernel;
    BOOL ImportMatched;
    BOOL PvpanicPresent;
    BOOL ShutdownSupported;
    BOOL HookActive;
    DWORD PvpanicMask;
    DWORD KernelTimestamp;
    DWORD KernelChecksum;
    DWORD BranchAddress;
    DWORD IatAddress;
    DWORD HalTarget;
    DWORD Result;
} QEMU_PROBE;

void QemuSetOutput(HANDLE Output, BOOL Console);
BOOL QemuProbeQuick(void);
BOOL QemuGetProbe(QEMU_PROBE *Probe);
LPCSTR QemuProbeStatusName(void);
BOOL QemuExportDiagnostics(LPCSTR Path);
BOOL QemuAction(LPCSTR Action);
BOOL QemuGetState(QEMU_STATE *State);
DWORD QemuLastError(void);
LPCSTR QemuLastFailure(void);
LPCSTR QemuCommandLineArgs(void);
#endif
