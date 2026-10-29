# Latence.app — experimental RTX 4070 Ti Windows port

A manual UEFI port for **one captured MSI RTX 4070 Ti AD104 / MSI Z490 system**, based on [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada). It runs before the unmodified Windows NVIDIA driver. This separate reference does not expand the Linux module's supported targets and is not a universal 4070 Ti installer.

| cuBLAS 4096² GEMM | Stock TFLOP/s | After TFLOP/s | Gain |
|---|---:|---:|---:|
| FP32 pedantic | 30.23 | 30.53 | +1.0% |
| FP16 with FP32 accumulation | 84.08 | 151.43 | **+80.1%** |
| BF16 with FP32 accumulation | 84.03 | 148.25 | **+76.4%** |

Medians of three independent processes per phase; numerical checks and complete output hashes match stock. The v0.5.4 transaction completed and Windows returned without an intervening reset/power cycle. This is one successful cold/manual launch and short GEMM tests, not application speedup or long-term stability qualification. Cold A/B/A repetition remains pending. See the [report](../../docs/ad104-windows-latence-app.md) and [sanitized evidence](../../docs/evidence/ad104-windows/README.md).

A second captured board — an RTX 4080 Laptop (`10de:27a0`, AD104, 58 SM) whose stock state was a clean 50 % cap on every tensor path — runs the same port with automatic unlock and no boot menu: FP16 73.6 → 146.9 T/s, INT8 unchanged, 9 s from application entry to the Windows kernel. See [`docs/ad104-4080-laptop-windows.md`](../../docs/ad104-4080-laptop-windows.md).

## Exact profile and containment

Tested: MSI RTX 4070 Ti `10de:2782`, subsystem `1462:5132`, A1, VBIOS 95.04.3c.40.fd; MSI MPG Z490 GAMING PLUS/MS-7C75 BIOS A.E0; i9-10850K; Windows build 26200.9168; driver 616.56; configured power limit 285 W. Actual preboot framebuffer is 12,282 MiB.

ROM, FWSEC signature versions, asset hashes, loaded graphics driver code, device paths, PCI/BARs, cold registers and memory geometry are pinned. A PCI ID match alone is insufficient. Do not remove guards to make another system boot. This application writes GPU registers. On failure it clears bus master, reconnects the console/GOP/NVIDIA drivers it detached and returns control to the firmware, so a failed run still boots a stock Windows instead of parking the machine.

The adapter releases exact TextOut/GraphicsConsole/NVIDIA GOP owners using UEFI callbacks, takes an exclusive PCI claim, and restores only missing original IO/MEM decode bits. It never patches driver code or guessed globals. USB additions/removals are allowed only within the reviewed ownership/path rules; host/GPU ownership, virtual console callbacks and non-USB invariants remain mandatory. The core requires payload completion, HALT, permissions, single selector writes, one FLR and post-FLR full-rate readback. Uncertain firmware-visible DMA mappings are retained until power-off. Windows starts only after successful cleanup and fresh final checks.

## Source-only build

Requires Windows x64, GCC/MinGW-w64 with `ar` and `nm` (tested GCC 15.2), Python 3 and MSVC x64 linker tools (tested VS 2022 Build Tools). `vswhere` discovers MSVC; use `-VcVars64` to select its setup script explicitly. No GPU access, driver install, internal ESP write or boot-variable change occurs during build/test.

The tested PC's ESP GUID is deliberately excluded from Git. Create the ignored local header and enter a reviewed GUID:

```powershell
New-Item -ItemType Directory -Force local
Copy-Item boot_profile.example.h local/boot_profile.h
# Review the real ESP GUID and replace the deliberately unusable zero example.
.\Build-Experimental.ps1 -Python python
```

The GUID is only one check: this profile also requires ESP partition 2, start LBA 1,085,440, size 202,752 sectors and the pinned 3,086,728-byte Microsoft boot manager. A different layout or boot-manager hash needs independent capture and review. No tool here automatically widens a hardware profile.

Output: `build/AD104-EXPERIMENTAL.EFI`. The build verifies x64 EFI entry, relocations, no imports/TLS/CLR and per-function stack frames. The source has moved on since the 2026-10-24 EFI (automatic unlock, the gone-vs-wrong verdict, and firmware handoff instead of a failure halt), so the historical EFI of this tree no longer rebuilds byte-for-byte; the measured results below belong to the revision that produced them, and the 4080 Laptop numbers were taken on the current revision. No portable EFI binary is distributed. New build manifests describe an unexecuted local build; historical evidence is separate.

## Firmware and private tests

No NVIDIA firmware/payload, ROM, loaded driver binary or Windows executable is included. Generate assets locally with the upstream hash-pinned workflow in Linux/WSL:

```sh
# Repository root; use the pinned generators at 491925c1079435e260137b9f3526066e764de830.
make assets ASSETS=/your/private/ad104-assets
```

`profile.c` enforces the five AD102 asset names, sizes and hashes. Keep external inputs private and retain their licence notices. The [read-only probe](diagnostic/README.md) can capture local evidence; raw captures contain binary data and must not be published.

```powershell
python tools/extract_private_capture.py CAP0004.JSON --out local/capture
.\Test-Experimental.ps1 -IncludeCore `
  -Prom local/capture/ad104-physical-prom.bin `
  -Assets C:\private\ad104-assets `
  -DriverCode local/capture `
  -WindowsImage C:\private\bootmgfw.efi
```

The Windows image must be a private copy of the reviewed boot manager already on your own ESP. Tests simulate all GPU/EFI effects and perform no unlock. Fresh run of this tree (`Test-Experimental.ps1 -IncludeCore`, `CORE-EXIT=0`): 9335 checks across 48 ownership/USB cases, 17 admission cases and 90 transaction/port scenarios, plus the application-flow and boot-I/O suites.

## Manual experimental boot

This is a researcher-operated, board-specific reference. Use a separate FAT32 USB you control; preserve previous boot files. No formatting or automatic installation tool is supplied. Copy the reviewed local EFI to `EFI/BOOT/BOOTX64.EFI` and the five generated AD102 assets to `AD104-ASSETS/` on that USB. Keep internal ESP, BCD and BootOrder unchanged.

1. With Secure Boot disabled for the unsigned experimental EFI and recovery available, fully shut down, wait ten seconds and select the UEFI USB.
2. The application unlocks by itself after a 1-second window. **U** starts immediately; **S**, **Esc** or any failure selects stock Windows without a driver release/GPU transaction. A machine with no keyboard unlocks by default.
3. The display may retain a frozen last frame. Do not interact with peripherals. After four minutes without Windows, power off fully, wait ten seconds and choose Windows Boot Manager.
4. Inspect USB `AD104-UNLOCK/RUNxxxx.TXT`, confirm actual driver recovery, then verify numerical correctness and throughput. A selector write, frozen frame or pre-handoff success line alone proves no Windows gain.

There is no automatic retry and no boot-variable edit by the application itself. It does reconnect the drivers it detached before handing control back, because a firmware with no display driver left on the device would otherwise leave the machine black and force a power cycle (which resets the GPU and discards a successful unlock). Previous complete power-offs restored the stock state on the tested system.

## Credits and licence

**Latence.app**: AD104/MSI Windows integration, admission profile, ownership/USB handling, tests and Windows evidence. Published through [Bizcott](https://github.com/Bizcott). This is an independent contribution, not upstream endorsement.

Ada code/payload integration: [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada), building on [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) and [TOTORONG/AD103_win_unlocker](https://huggingface.co/TOTORONG/AD103_win_unlocker). UEFI strategy/compatibility: [impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK), pinned commits in `provenance.json`.

GPL-2.0-only unless a file states otherwise. Preserve [third-party notices](THIRD_PARTY_NOTICES.md), Windows upstream notices, EDK II licences, MIT diagnostic/PE-validator notices and original copyright headers.
