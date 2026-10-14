#include <ntddk.h>
#include "qemupwr.h"

#define PV_KERNEL_BASE 0x80100000UL
#define PV_DEBUG_PORT 0x00E9
#define PV_PVPANIC_PORT 0x0505
#define PV_PVPANIC_SHUTDOWN 0x04U

#define PV_NT351_STAMP 0x321A03D2UL
#define PV_NT351_CHECK 0x000D3B29UL
#define PV_NT351_SIZE  0x000BCD00UL
#define PV_NT4_STAMP   0x405977CDUL
#define PV_NT4_CHECK   0x000EACFFUL
#define PV_NT4_RUNTIME_SIZE_A 0x000E5000UL
#define PV_NT4_RUNTIME_SIZE_B 0x000DC000UL

typedef VOID (__stdcall *PV_FIRMWARE_FN)(ULONG Routine);
static QEMUPWR_REPORT g_Report;
static PDEVICE_OBJECT g_Device = NULL;
static PDRIVER_OBJECT g_Driver = NULL;
static volatile LONG *g_Iat = NULL;
static PV_FIRMWARE_FN g_Original = NULL;
static BOOLEAN g_Armed = FALSE;
static BOOLEAN g_IatInstalled = FALSE;
static UNICODE_STRING g_DeviceName;
static UNICODE_STRING g_SymbolicName;
static VOID PvUnload(PDRIVER_OBJECT DriverObject);

static VOID PvLog(const char *S)
{
    while (*S) {
        WRITE_PORT_UCHAR((PUCHAR)(ULONG)PV_DEBUG_PORT, (UCHAR)*S);
        ++S;
    }
}

static VOID PvLogHex(ULONG V)
{
    static const char H[] = "0123456789ABCDEF";
    int I;
    PvLog("0x");
    for (I = 7; I >= 0; --I)
        WRITE_PORT_UCHAR((PUCHAR)(ULONG)PV_DEBUG_PORT,
                         (UCHAR)H[(V >> (I * 4)) & 15UL]);
}

static BOOLEAN PvMapped(PUCHAR P, ULONG Count)
{
    ULONG I;
    if (Count == 0 || Count > 0x200000UL ||
        (ULONG)P > 0xFFFFFFFFUL - Count)
        return FALSE;
    for (I = 0; I < Count; I += 0x1000UL)
        if (!MmIsAddressValid(P + I)) return FALSE;
    return MmIsAddressValid(P + Count - 1);
}

static ULONG Pv32(PUCHAR P)
{
    return *(PULONG)P;
}

static USHORT Pv16(PUCHAR P)
{
    return *(PUSHORT)P;
}

static BOOLEAN PvRvaMapped(PUCHAR Base, ULONG ImageSize,
                           ULONG Rva, ULONG Count)
{
    if (Rva >= ImageSize || Count > ImageSize - Rva) return FALSE;
    return PvMapped(Base + Rva, Count);
}

static BOOLEAN PvEqual(PUCHAR P, const char *S, ULONG Max)
{
    ULONG I;
    for (I = 0; I < Max; ++I) {
        if ((UCHAR)S[I] != P[I]) return FALSE;
        if (S[I] == 0) return TRUE;
    }
    return FALSE;
}

static ULONG PvFindHalImport(PUCHAR Base, ULONG ImageSize, ULONG Opt)
{
    ULONG ImportRva, ImportSize, Desc, N, Oft, First, NameRva, T;
    ULONG Lookup, HintName, Entry;
    if (Pv32(Base + Opt + 92) < 2UL) return 0;
    ImportRva = Pv32(Base + Opt + 96 + 8);
    ImportSize = Pv32(Base + Opt + 96 + 8 + 4);
    if (ImportRva == 0 || ImportSize < 20) return 0;
    if (ImportSize > 0x10000UL) ImportSize = 0x10000UL;

    for (N = 0; N < ImportSize / 20UL && N < 128UL; ++N) {
        Desc = ImportRva + N * 20UL;
        if (!PvRvaMapped(Base, ImageSize, Desc, 20)) return 0;
        Oft = Pv32(Base + Desc);
        NameRva = Pv32(Base + Desc + 12);
        First = Pv32(Base + Desc + 16);
        if (Oft == 0 && First == 0) break;
        if (Oft == 0 || First == 0) continue;
        if (!PvRvaMapped(Base, ImageSize, NameRva, 8)) continue;
        if (!PvEqual(Base + NameRva, "HAL.dll", 8)) continue;
        for (T = 0; T < 512UL; ++T) {
            Lookup = Oft + T * 4UL;
            if (!PvRvaMapped(Base, ImageSize, Lookup, 4)) break;
            Entry = Pv32(Base + Lookup);
            if (Entry == 0) break;
            if ((Entry & 0x80000000UL) != 0) continue;
            HintName = Entry;
            if (!PvRvaMapped(Base, ImageSize, HintName, 2 + 20)) continue;
            if (PvEqual(Base + HintName + 2, "HalReturnToFirmware", 20))
                return (ULONG)(Base + First + T * 4UL);
        }
    }
    return 0;
}

static BOOLEAN PvSignature(PUCHAR B)
{
    static const UCHAR Head[] = {0x84,0xDB,0x74,0x1C,0x6A,0x00,
                                 0x6A,0xFF,0x6A,0x00,0xE8};
    static const UCHAR Middle[] = {0x83,0xFE,0x01,0x75,0x04,
                                   0x6A,0x03,0xEB,0x02,0x6A,0x01,
                                   0xFF,0x15};
    static const UCHAR Tail[] = {0x33,0xC0,0x5E,0x5B,0xC2,0x04,0x00};
    ULONG I;
    for (I = 0; I < sizeof(Head); ++I)
        if (B[-2 + I] != Head[I]) return FALSE;
    for (I = 0; I < sizeof(Middle); ++I)
        if (B[13 + I] != Middle[I]) return FALSE;
    for (I = 0; I < sizeof(Tail); ++I)
        if (B[30 + I] != Tail[I]) return FALSE;
    return TRUE;
}

static VOID PvScan(VOID)
{
    PUCHAR Base = (PUCHAR)(ULONG)PV_KERNEL_BASE;
    PUCHAR Nt, Opt, Sec, Candidate = NULL;
    ULONG E_lfanew, Sections, OptSize, ImageSize, I, S, Size, Rva;
    ULONG FoundIat = 0;
    ULONG ImportSlot = 0;
    ULONG ScannedSections = 0;

    RtlZeroMemory(&g_Report, sizeof(g_Report));
    g_Report.Signature = QEMUPWR_SIGNATURE;
    g_Report.Version = QEMUPWR_REPORT_VERSION;
    g_Report.Size = sizeof(g_Report);
    g_Report.KernelBase = PV_KERNEL_BASE;
    g_Report.Result = QEMUPWR_BAD_KERNEL_BASE;
    g_Report.PvpanicMask = READ_PORT_UCHAR((PUCHAR)(ULONG)PV_PVPANIC_PORT);

    if (!PvMapped(Base, 0x100)) return;
    g_Report.Result = QEMUPWR_BAD_PE;
    if (Pv16(Base) != 0x5A4D) return;
    E_lfanew = Pv32(Base + 0x3C);
    if (E_lfanew > 0x1000UL || !PvMapped(Base + E_lfanew, 0x100)) return;
    Nt = Base + E_lfanew;
    if (Pv32(Nt) != 0x00004550UL || Pv16(Nt + 4) != 0x014C) return;
    OptSize = Pv16(Nt + 20);
    Sections = Pv16(Nt + 6);
    if (OptSize < 0xE0UL || Sections == 0 || Sections > 32) return;
    Opt = Nt + 24;
    if (!PvMapped(Opt, OptSize) || Pv16(Opt) != 0x010B) return;

    ImageSize = Pv32(Opt + 56);
    g_Report.KernelTimestamp = Pv32(Nt + 8);
    g_Report.KernelChecksum = Pv32(Opt + 64);
    g_Report.KernelImageSize = ImageSize;
    g_Report.KernelPreferredBase = Pv32(Opt + 28);
    g_Report.Result = QEMUPWR_BAD_IMAGE;
    if (ImageSize < 0x20000UL || ImageSize > 0x200000UL ||
        g_Report.KernelPreferredBase != PV_KERNEL_BASE) return;

    if ((g_Report.KernelTimestamp == PV_NT351_STAMP &&
         g_Report.KernelChecksum == PV_NT351_CHECK &&
         ImageSize == PV_NT351_SIZE) ||
        (g_Report.KernelTimestamp == PV_NT4_STAMP &&
         g_Report.KernelChecksum == PV_NT4_CHECK &&
         (ImageSize == PV_NT4_RUNTIME_SIZE_A ||
          ImageSize == PV_NT4_RUNTIME_SIZE_B)))
        g_Report.Flags |= QEMUPWR_FLAG_KNOWN;

    g_Report.Result = QEMUPWR_NO_TEXT;
    Sec = Opt + OptSize;
    if (!PvMapped(Sec, Sections * 40UL)) return;

    ImportSlot = PvFindHalImport(Base, ImageSize, (ULONG)(Opt - Base));
    g_Report.HalImportFromTable = ImportSlot;
    if (ImportSlot != 0) g_Report.Flags |= QEMUPWR_FLAG_IMPORT;

    g_Report.Result = QEMUPWR_NO_BRANCH;
    for (S = 0; S < Sections; ++S) {
        PUCHAR Header = Sec + S * 40UL;
        ULONG Characteristics = Pv32(Header + 36);
        if ((Characteristics & 0x20000020UL) != 0x20000020UL)
            continue;
        Rva = Pv32(Header + 12);
        Size = Pv32(Header + 8);
        if (Size < 64 || Rva >= ImageSize || Size > ImageSize - Rva)
            continue;
        ++ScannedSections;
        for (I = 24; I + 40 < Size; ++I) {
            PUCHAR Current = Base + Rva + I;
            if (!MmIsAddressValid(Current)) {
                I += 0xFFFUL - ((ULONG)Current & 0xFFFUL);
                continue;
            }
            if (ImportSlot != 0 && Current[0] == 0xFF &&
                PvMapped(Current - 24, 48) &&
                Current[1] == 0x15 && Pv32(Current + 2) == ImportSlot) {
                ++g_Report.HalCallCandidates;
                if (g_Report.FirstHalCall == 0) {
                    ULONG J;
                    g_Report.FirstHalCall = (ULONG)Current;
                    for (J = 0; J < sizeof(g_Report.HalCallContext); ++J)
                        g_Report.HalCallContext[J] = Current[-24 + J];
                }
            }
            if (Current[0] != 0x74) continue;
            if (!PvMapped(Current - 8, 48)) continue;
            if (Current[1] != 0x1C || !PvSignature(Current)) continue;
            ++g_Report.BranchMatches;
            if (Candidate == NULL) {
                Candidate = Current;
                FoundIat = Pv32(Current + 26);
                g_Report.KernelTextRva = Rva;
                g_Report.KernelTextSize = Size;
            }
        }
    }
    if (ScannedSections == 0) {
        g_Report.Result = QEMUPWR_NO_TEXT;
        return;
    }
    if (g_Report.BranchMatches == 0) return;
    g_Report.Result = QEMUPWR_AMBIGUOUS_BRANCH;
    if (g_Report.BranchMatches != 1) return;

    g_Report.BranchAddress = (ULONG)Candidate;
    g_Report.CallAddress = (ULONG)(Candidate + 24);
    g_Report.IatAddress = FoundIat;
    for (I = 0; I < sizeof(g_Report.BranchContext); ++I)
        g_Report.BranchContext[I] = Candidate[-8 + I];
    if (FoundIat < PV_KERNEL_BASE ||
        FoundIat > PV_KERNEL_BASE + ImageSize - 4UL ||
        (FoundIat & 3UL) != 0 ||
        !PvMapped((PUCHAR)(ULONG)FoundIat, 4)) {
        g_Report.Result = QEMUPWR_BAD_IAT;
        return;
    }
    for (I = 0; I < sizeof(g_Report.IatContext); ++I) {
        ULONG Around = FoundIat - 8UL + I;
        if (Around >= PV_KERNEL_BASE &&
            Around < PV_KERNEL_BASE + ImageSize &&
            PvMapped((PUCHAR)(ULONG)Around, 1))
            g_Report.IatContext[I] = *(PUCHAR)(ULONG)Around;
    }
    g_Report.HalTarget = *(PULONG)(ULONG)FoundIat;
    g_Report.Result = QEMUPWR_BAD_HAL_TARGET;
    if (g_Report.HalTarget == 0 ||
        g_Report.HalTarget == 0xFFFFFFFFUL ||
        !PvMapped((PUCHAR)(ULONG)g_Report.HalTarget, 1)) return;

    if (ImportSlot != 0 && ImportSlot != FoundIat) {
        g_Report.Result = QEMUPWR_BAD_IAT;
        return;
    }
    if (!(g_Report.Flags & QEMUPWR_FLAG_KNOWN) && ImportSlot == 0) {
        g_Report.Result = QEMUPWR_NOT_VERIFIED;
        return;
    }
    g_Report.Result = QEMUPWR_OK;
    g_Report.Flags |= QEMUPWR_FLAG_COMPATIBLE;
}

static VOID __stdcall PvHalHook(ULONG Routine)
{
    PvLog("QEMUPWR: HalReturnToFirmware routine=");
    PvLogHex(Routine);
    if (Routine == 1) PvLog(" (POWERDOWN)");
    else if (Routine == 3) PvLog(" (REBOOT)");
    PvLog("\r\n");

    if (Routine == 1) {
        UCHAR Capabilities = READ_PORT_UCHAR((PUCHAR)(ULONG)PV_PVPANIC_PORT);
        if (Capabilities != 0xFFU &&
            (Capabilities & PV_PVPANIC_SHUTDOWN) != 0) {
            PvLog("QEMUPWR: pvpanic shutdown requested at 0x505 (0x04)\r\n");
            WRITE_PORT_UCHAR((PUCHAR)(ULONG)PV_PVPANIC_PORT,
                             PV_PVPANIC_SHUTDOWN);
            for (;;) {
                __asm {
                    cli
                    hlt
                }
            }
        }
        PvLog("QEMUPWR: pvpanic shutdown unavailable; HAL fallback\r\n");
    }
    if (g_Original != NULL) g_Original(Routine);
}

static NTSTATUS PvVerifyFileImport(const QEMUPWR_FILE_IMPORT *File)
{
    if (File == NULL || File->Size != sizeof(*File))
        return STATUS_INVALID_DEVICE_REQUEST;
    if (g_Report.Result != QEMUPWR_NOT_VERIFIED ||
        g_Report.BranchMatches != 1 || g_Report.IatAddress == 0 ||
        g_Report.HalTarget == 0)
        return STATUS_NOT_SUPPORTED;
    if (File->KernelTimestamp != g_Report.KernelTimestamp ||
        File->KernelChecksum != g_Report.KernelChecksum ||
        File->KernelImageSize != g_Report.KernelImageSize ||
        File->HalIatAddress != g_Report.IatAddress)
        return STATUS_NOT_SUPPORTED;

    g_Report.Result = QEMUPWR_OK;
    g_Report.Flags |= QEMUPWR_FLAG_FILE_IMPORT | QEMUPWR_FLAG_COMPATIBLE;
    return STATUS_SUCCESS;
}

static NTSTATUS PvArm(BOOLEAN Experimental)
{
    LONG Previous;
    if (g_Armed) return STATUS_SUCCESS;
    if (g_Report.Result != QEMUPWR_OK ||
        !(g_Report.Flags & QEMUPWR_FLAG_COMPATIBLE))
        return STATUS_NOT_SUPPORTED;
    if (!(g_Report.Flags & QEMUPWR_FLAG_KNOWN) && !Experimental)
        return STATUS_NOT_SUPPORTED;
    if (g_Report.IatAddress == 0)
        return STATUS_NOT_SUPPORTED;
    if (g_Report.PvpanicMask == 0xFFU ||
        (g_Report.PvpanicMask & PV_PVPANIC_SHUTDOWN) == 0) {
        g_Report.Result = QEMUPWR_NO_PVPANIC_SHUTDOWN;
        return STATUS_NOT_SUPPORTED;
    }

    g_Iat = (volatile LONG *)(ULONG)g_Report.IatAddress;
    if ((ULONG)*g_Iat != g_Report.HalTarget) {
        g_Report.Result = QEMUPWR_ALREADY_MODIFIED;
        g_Iat = NULL;
        return STATUS_NOT_SUPPORTED;
    }
    g_Original = (PV_FIRMWARE_FN)(ULONG)*g_Iat;
    g_Driver->DriverUnload = NULL;
    Previous = InterlockedExchange((PLONG)g_Iat, (LONG)PvHalHook);
    if ((ULONG)Previous != (ULONG)g_Original) {
        InterlockedExchange((PLONG)g_Iat, Previous);
        g_Report.Result = QEMUPWR_IAT_CHANGED;
        g_Iat = NULL;
        g_Original = NULL;
        g_Driver->DriverUnload = PvUnload;
        return STATUS_UNSUCCESSFUL;
    }
    g_IatInstalled = TRUE;
    g_Armed = TRUE;
    g_Report.Flags |= QEMUPWR_FLAG_ARMED;
    PvLog("QEMUPWR: HAL hook installed; HAL=");
    PvLogHex((ULONG)g_Original);
    PvLog("; IAT=");
    PvLogHex(g_Report.IatAddress);
    PvLog("\r\n");
    return STATUS_SUCCESS;
}

static NTSTATUS PvDisarm(VOID)
{
    if (!g_IatInstalled && !g_Armed)
        return STATUS_SUCCESS;
    if (g_Iat == NULL || g_Original == NULL ||
        (ULONG)*g_Iat != (ULONG)PvHalHook)
        return STATUS_UNSUCCESSFUL;

    InterlockedExchange((PLONG)g_Iat, (LONG)g_Original);
    if ((ULONG)*g_Iat != (ULONG)g_Original)
        return STATUS_UNSUCCESSFUL;
    g_IatInstalled = FALSE;
    g_Armed = FALSE;
    g_Report.Flags &= ~QEMUPWR_FLAG_ARMED;
    PvLog("QEMUPWR: HAL import restored at ");
    PvLogHex(g_Report.IatAddress);
    PvLog("\r\n");
    g_Iat = NULL;
    g_Original = NULL;
    g_Driver->DriverUnload = PvUnload;
    return STATUS_SUCCESS;
}

static NTSTATUS PvDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Information = 0;
    UNREFERENCED_PARAMETER(DeviceObject);
    if (Stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
        ULONG Control = Stack->Parameters.DeviceIoControl.IoControlCode;
        if (Control == QEMUPWR_IOCTL_QUERY) {
            if (Stack->Parameters.DeviceIoControl.OutputBufferLength <
                sizeof(g_Report)) Status = STATUS_BUFFER_TOO_SMALL;
            else {
                RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                              &g_Report, sizeof(g_Report));
                Information = sizeof(g_Report);
            }
        } else if (Control == QEMUPWR_IOCTL_ARM ||
                   Control == QEMUPWR_IOCTL_ARM_EXPERIMENTAL) {
            Status = PvArm((BOOLEAN)(Control == QEMUPWR_IOCTL_ARM_EXPERIMENTAL));
        } else if (Control == QEMUPWR_IOCTL_DISARM) {
            Status = PvDisarm();
        } else if (Control == QEMUPWR_IOCTL_VERIFY_FILE) {
            if (Stack->Parameters.DeviceIoControl.InputBufferLength <
                sizeof(QEMUPWR_FILE_IMPORT))
                Status = STATUS_BUFFER_TOO_SMALL;
            else
                Status = PvVerifyFileImport(
                    (const QEMUPWR_FILE_IMPORT *)Irp->AssociatedIrp.SystemBuffer);
        } else Status = STATUS_INVALID_DEVICE_REQUEST;
    }
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static VOID PvUnload(PDRIVER_OBJECT DriverObject)
{
    if (g_IatInstalled || g_Armed) {
        PvLog("QEMUPWR: ERROR: unload requested with active hook\r\n");
        return;
    }
    IoDeleteSymbolicLink(&g_SymbolicName);
    if (g_Device != NULL) IoDeleteDevice(g_Device);
    UNREFERENCED_PARAMETER(DriverObject);
}

static BOOLEAN PvAutoArmEnabled(PUNICODE_STRING RegistryPath)
{
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING ValueName;
    HANDLE Key;
    NTSTATUS Status;
    ULONG Size = 0;
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Value;
    BOOLEAN Enabled = FALSE;

    InitializeObjectAttributes(&Attributes, RegistryPath,
                               OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwOpenKey(&Key, KEY_QUERY_VALUE, &Attributes);
    if (!NT_SUCCESS(Status)) return FALSE;
    RtlInitUnicodeString(&ValueName, L"AutoArm");
    Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation,
                             Buffer, sizeof(Buffer), &Size);
    if (NT_SUCCESS(Status)) {
        Value = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
        if (Value->Type == REG_DWORD && Value->DataLength == sizeof(ULONG))
            Enabled = (*(PULONG)Value->Data == 1UL);
    }
    ZwClose(Key);
    return Enabled;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;
    RtlInitUnicodeString(&g_DeviceName, L"\\Device\\QEMUPWR");
    RtlInitUnicodeString(&g_SymbolicName, L"\\DosDevices\\QEMUPWR");
    Status = IoCreateDevice(DriverObject, 0, &g_DeviceName,
                            FILE_DEVICE_UNKNOWN, 0, FALSE, &g_Device);
    if (!NT_SUCCESS(Status)) return Status;
    Status = IoCreateSymbolicLink(&g_SymbolicName, &g_DeviceName);
    if (!NT_SUCCESS(Status)) {
        IoDeleteDevice(g_Device);
        g_Device = NULL;
        return Status;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE] = PvDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = PvDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = PvDispatch;
    g_Driver = DriverObject;
    DriverObject->DriverUnload = PvUnload;
    PvScan();
    if (PvAutoArmEnabled(RegistryPath)) {
        if (!NT_SUCCESS(PvArm(FALSE))) {
            PvLog("QEMUPWR: Automatic hook failed; result=");
            PvLogHex(g_Report.Result);
            PvLog("\r\n");
        }
    }
    return STATUS_SUCCESS;
}
