# AD104 in an RTX 4080 Laptop (`10de:27a0`) — UEFI port, automatic boot, measured result

This is a second board profile for the same Windows port. It is **not** a universal
installer: every guard (ROM, asset hashes, ESP layout, boot-manager hash, driver code
capture) still has to match a machine that was captured and reviewed.

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 4080 Laptop, AD104, `10de:27a0`, 58 SM |
| Chip id at runtime | `0x194000a1` |
| Enforced power limit | 150 W (board drew 50 W during the measurement) |
| Platform | laptop board, UEFI firmware with MSI-style console/GOP ownership |
| OS at measurement | Windows, unmodified NVIDIA driver |

The machine is unique enough that the desktop MSI 4070 Ti profile in
[`windows/ad104-msi/README.md`](../windows/ad104-msi/README.md) does not apply to it.
No boot variable, no firmware blob and no driver binary from this machine is included
in this repository.

## Result

The stock state of this card was a clean **50 % cap on every tensor path** except INT8.
After the transaction every capped path moved to ~97 % of its architectural peak.
INT8 was never capped and did not move — which is what a rate-limit removal looks like,
and it rules out a clock or power change.

| mode | stock T/s | after T/s | stock FLOP/clk/SM | after FLOP/clk/SM | peak | gain |
|---|---:|---:|---:|---:|---:|---:|
| FP16 / FP32 acc | 73.6 | 146.9 | 498 | 994 | 1024 | +100 % |
| BF16 / FP32 acc | 73.6 | 143.4 | 498 | 993 | 1024 | +95 % |
| TF32 / FP32 acc | 36.8 | 73.5 | 249 | 497 | 512 | +100 % |
| FP8 E4M3 / FP32 acc | 147.1 | 293.8 | 995 | 1986 | 2048 | +100 % |
| INT8 / INT32 acc | 293.0 | 293.2 | 1982 | 1983 | 2048 | 0 % |

Clock and power did not change: SM clock 2550 MHz before and after, board power
43.85 W → 50.5–52.0 W at a third of the 150 W cap, pstate P0. The gain is issue rate,
not clocks.

Measured with [`mma_bench.cu`](../windows/ad104-msi/bench/mma_bench.cu), a
register-resident MMA probe with no memory traffic, so the number cannot be explained
by cache or bandwidth effects. Full control/after tables:
[`evidence/ad104-4080-laptop/`](evidence/ad104-4080-laptop/).

**Read the probe correctly.** `mma_bench.exe` ramps the SM clock from idle: on this
machine the first run reported FP16 79.9 T/s and the *second* run still reported
79.9 T/s; only the third run read 146.95 T/s. Discarding just the first run is not
enough on this board. Take the third.

## The registers that changed (RUN0030, after a cold start)

| register | before | after | expected | valid |
|---|---|---|---|---|
| `WPR_CFG[0x1fa7cc]` | `0x0004cb8f` | `0xfffff0ff` | `0xfffff0ff` | 1 |
| `FEAT[0x823804]` | `0xffffff8f` | `0xffffffff` | `0xffffffff` | 1 |
| `WPR[0x1fa7c4]` | `0x0004cb8f` | `0xffffffff` | `0xffffffff` | 1 |
| `FBPA[0x9a0148]` | `0xffffff0f` | `0xffffffff` | `0xffffffff` | 1 |
| `readout1[0x823818]` | `0x00410080` | `0` (fields all 0, `full=1`) | — | 1 |

`Transaction result=0`, `TRANSACTION OK: hardware readbacks passed, DMA released`.

## Automatic boot

The researcher-operated flow used to require pressing `U` at every start and then
picking Windows from a boot menu. Both are gone; a cold start now unlocks and boots
Windows with no interaction. Measured end-to-end on a cold start (power off, 10 s, on):

```
[T] ENTRY          12:50:36
[T] UNLOCK-DONE    12:50:41   +5 s   asset load, four register writes, readback
[T] FINAL-CHECK    12:50:41   +0 s
[T] HANDOFF-ok     12:50:43   +2 s   reconnect the detached console/GOP/NV drivers
    Windows kernel 12:50:45   +2 s   firmware -> {bootmgr} -> Windows
```

9 seconds from application entry to the Windows kernel, 7 of them in the application.
The previous flow spent 13 seconds after handoff because the Windows Boot Manager menu
was waiting for a keypress.

How it is wired:

- `{fwbootmgr}` `displayorder` = this application, then `{bootmgr}`; `timeout 2`
  (kept so a hotkey still reaches the firmware setup).
- `{bootmgr}` `timeout 0` **and** `displaybootmenu No`. `timeout 0` alone does not hide
  the "Windows 11 / recovery" menu; `displaybootmenu Yes` forces it regardless of the
  timeout. This was the entire reason a keypress was still needed.
- Recovery is not removed: `recoveryenabled Yes` and `recoverysequence` stay, so
  Shift+Restart, Settings → Recovery and F8 still reach WinRE. Only the blocking menu
  is gone.
- The choice prompt inside the application defaults to **unlock** and waits 1 second;
  `U` starts immediately, `S`/`Esc` skips this boot. A machine with no keyboard
  attached unlocks instead of aborting.

## Four things that cost 18 reboots to learn

These are the traps that are easy to repeat; all four are fixed in this tree.

1. **Never call `StartImage` on `bootmgfw.efi`.** It returns
   `STATUS_INVALID_IMAGE_FORMAT (0xc000007b)` and bootmgr then reports the image as
   corrupt — the "file is missing or contains errors" screen. Hand the firmware
   `EFI_SUCCESS` instead and let `{fwbootmgr}` walk on to `{bootmgr}`.
2. **"Cannot read it" is not "the value is wrong".** The last pre-Windows check runs
   after Windows may already own the device; every register then reads `0xffffffff`.
   Treating that as failure marked every *successful* run as failed and sent it down
   the containment path. The verdict is now a pure function of
   `(gone, five register values, fault, verified_at_write)`:
   gone + verified at write time → accept; readable but wrong → refuse; failed
   transaction → refuse. Four assertions cover the branches.
3. **`RuntimeServices->GetTime` returns `Nanosecond = 0` here**, so timestamps have
   one-second resolution. Do not build a benchmark on that field.
4. **`strings` vs `strings -el`.** Menu text is `CHAR16` (`-el`), log strings are ASCII.
   Checking a deployed binary with the wrong one produces a confident "the string is
   not in there" that is false.

## Failure behaviour

Every exit returns `EFI_SUCCESS`. On failure the application clears bus master, drops
unverified mappings, reconnects the drivers it detached with `Recursive=TRUE`, stamps
`[T] HANDOFF-fail`, and lets the firmware start Windows. A failed unlock therefore
boots a stock Windows; it does not hang, and it does not require a power cycle (which
would reset the GPU and discard a successful unlock).

Failure evidence lands in `BOOTTRACE.TXT` on the application's own ESP, because the USB
log is closed before `StartImage` and `GuLog` cannot be used after that.

## Not included

No VBIOS/ROM dump, no GSP firmware, no captured driver code, no Windows boot manager
copy, no firmware-visible build outputs, and no ESP GUID from this machine. Those are
ignored by `.gitignore` and must be generated or captured locally, as described in
[`windows/ad104-msi/README.md`](../windows/ad104-msi/README.md).
