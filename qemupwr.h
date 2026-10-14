#ifndef QEMUPWR_H
#define QEMUPWR_H

#define QEMUPWR_IOCTL_QUERY 0x00222000UL
#define QEMUPWR_IOCTL_ARM   0x00222004UL
#define QEMUPWR_IOCTL_ARM_EXPERIMENTAL 0x00222008UL
#define QEMUPWR_IOCTL_DISARM 0x0022200CUL
#define QEMUPWR_IOCTL_VERIFY_FILE 0x00222010UL
#define QEMUPWR_SIGNATURE 0x52575051UL
#define QEMUPWR_REPORT_VERSION 2UL

#define QEMUPWR_OK 0UL
#define QEMUPWR_BAD_KERNEL_BASE 1UL
#define QEMUPWR_BAD_PE 2UL
#define QEMUPWR_BAD_IMAGE 3UL
#define QEMUPWR_NO_TEXT 4UL
#define QEMUPWR_NO_BRANCH 5UL
#define QEMUPWR_AMBIGUOUS_BRANCH 6UL
#define QEMUPWR_BAD_IAT 7UL
#define QEMUPWR_BAD_HAL_TARGET 8UL
#define QEMUPWR_NOT_VERIFIED 9UL
#define QEMUPWR_ALREADY_MODIFIED 10UL
#define QEMUPWR_ALREADY_ARMED 11UL
#define QEMUPWR_WRITE_FAILED 12UL
#define QEMUPWR_IAT_CHANGED 13UL
#define QEMUPWR_NO_PVPANIC_SHUTDOWN 14UL
#define QEMUPWR_WINLOGON_UNSUPPORTED 15UL

#define QEMUPWR_FLAG_KNOWN 1UL
#define QEMUPWR_FLAG_IMPORT 2UL
#define QEMUPWR_FLAG_COMPATIBLE 4UL
#define QEMUPWR_FLAG_ARMED 8UL
#define QEMUPWR_FLAG_FILE_IMPORT 32UL

#pragma pack(4)
typedef struct _QEMUPWR_FILE_IMPORT {
    unsigned long Size;
    unsigned long KernelTimestamp;
    unsigned long KernelChecksum;
    unsigned long KernelImageSize;
    unsigned long HalIatAddress;
} QEMUPWR_FILE_IMPORT;

typedef struct _QEMUPWR_REPORT {
    unsigned long Signature;
    unsigned long Version;
    unsigned long Size;
    unsigned long Flags;
    unsigned long Result;
    unsigned long KernelBase;
    unsigned long KernelTimestamp;
    unsigned long KernelChecksum;
    unsigned long KernelImageSize;
    unsigned long KernelPreferredBase;
    unsigned long KernelTextRva;
    unsigned long KernelTextSize;
    unsigned long BranchMatches;
    unsigned long BranchAddress;
    unsigned long CallAddress;
    unsigned long IatAddress;
    unsigned long HalTarget;
    unsigned long PvpanicMask;
    unsigned long HalImportFromTable;
    unsigned long HalCallCandidates;
    unsigned long FirstHalCall;
    unsigned char BranchContext[48];
    unsigned char IatContext[16];
    unsigned char HalCallContext[48];
} QEMUPWR_REPORT;
#pragma pack()

#endif
