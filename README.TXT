QEMU pvpanic Driver 1.4 - Source
================================

QEMUPWR enables automatic QEMU pvpanic power-off on these tested non-ACPI compliant x86 guests:
  Windows NT 3.51 SP5 (build 1057)
  Windows NT 4.0 SP6 (build 1381)

Runtime
-------
QEMUPWR.EXE provides the native Win32 GUI, command-line interface,
installation/removal and the QEMUPWRAgent service. QEMUPWR.SYS installs
an in-memory hook on the kernel HalReturnToFirmware import.

QEMUPWRAgent remaps the Winlogon Shutdown action (0x05) to Power Off
(0x0A). The Restart action (0x0B) is unchanged. On HAL POWERDOWN (1),
the kernel driver issues QEMU pvpanic event 0x04 at I/O port 0x505.
HAL REBOOT (3) is passed to the original HAL.

Kernel and Winlogon image profiles are listed in KERNEL_MATCHES.TXT.
The shutdown branch is checked as part of kernel identification; it is
not patched. Other kernel/Winlogon builds have not been validated.

Build requirements
------------------
- Windows NT 3.51 DDK (3.51.1057.1)
  https://winworldpc.com/product/windows-sdk-ddk/nt-3x
- Microsoft Visual C++ 2.2 (x86)
  https://winworldpc.com/product/visual-c/2x
- Win32 SDK headers and import libraries

Use an NT 3.51 DDK Checked Build Environment with the compiler, linker,
headers and libraries configured. Run BUILDALL.CMD from this directory.
The script builds the driver with DDK BUILD.EXE, links the Win32
application with CL/LINK and copies QEMUPWR.EXE and QEMUPWR.SYS to DIST.

Distribution
------------
Only QEMUPWR.EXE and QEMUPWR.SYS are required at runtime. End-user
instructions are maintained with the binaries on the main branch.

QEMUPWR.EXE accepts SCAN, INSTALL, STATUS and REMOVE as command-line
actions. Diagnostic reports are written only through the GUI Save Report
action. The optional QEMU debug console uses I/O port 0xE9.

Microsoft compiler, SDK, DDK and operating system files are not included.
