# Comparison run — RTX 4080 Laptop (10de:27a0) AFTER the AD104 unlock

Measured 2026-10-28 22:23 local. Same binary as the control arm:
`bench/\mma_bench.exe` (unchanged since 2026-10-26 23:43),
same default arguments (iters=60000 x 8 mma per thread, grid=464 x 256 thr, 58 SM).

Control arm: `BASELINE-stock.md`, measured 2026-10-27 02:17 on the same machine,
same binary, same arguments, before any unlock attempt.

Unlock provenance: UEFI run RUN0027 at 2026-10-28 20:43 completed the GPU transaction —
`Transaction result=0`, `TRANSACTION OK`, four permission registers read back `valid=1`
(WPR_CFG / FBPA / WPR / FEAT). The process then failed its own final self-check
(`stage=final-Windows-verification status=0x6`) and went down the containment path,
but `GuClearMaster()` only clears the PCI BUS_MASTER bit and does not touch those
four registers. This measurement is the first independent confirmation that the
transaction had a real hardware effect.

## Raw wall-clock runs (three consecutive invocations, 22:23:33-35)

| mode | RUN1 ms | RUN1 T/s | RUN2 ms | RUN2 T/s | RUN3 ms | RUN3 T/s |
|---|---:|---:|---:|---:|---:|---:|
| FP16 / FP32 acc | 91.417 | 79.83 | 49.796 | 146.56 | 49.670 | 146.93 |
| BF16 / FP32 acc | 53.039 | 137.60 | 49.696 | 146.86 | 49.676 | 146.91 |
| TF32 / FP32 acc | 50.434 | 72.35 | 49.679 | 73.45 | 49.674 | 73.46 |
| INT8 / INT32 acc | 49.779 | 293.22 | 49.810 | 293.04 | 49.773 | 293.25 |
| FP8 E4M3 / FP32 acc | 49.698 | 293.70 | 49.696 | 293.71 | 49.695 | 293.72 |

RUN1 again shows the clock ramp (FP16 79.83, BF16 137.60). As before, only RUN2/RUN3 count.

## FLOP per SM per clock — the clock-independent number

Derived as T/s / 58 SM / 2.55e9, using the SM clock actually sampled during the runs
(2550 and 2535 MHz, see telemetry). Full-rate reference is the architectural peak with
FP32 accumulate.

| mode | stock FLOP/clk/SM | now FLOP/clk/SM | integer peak | stock | now |
|---|---:|---:|---:|---|---|
| FP16 / FP32 acc | 498 | 994 | 1024 | 49 % | **97 %** |
| BF16 / FP32 acc | 498 | 993 | 1024 | 49 % | **97 %** |
| TF32 / FP32 acc | 249 | 497 | 512 | 49 % | **97 %** |
| FP8 E4M3 / FP32 acc | 995 | 1986 | 2048 | 49 % | **97 %** |
| INT8 / INT32 acc | 1982 | 1983 | 2048 | 97 % | 97 % (unchanged) |

Correction to the control document: FP8's architectural peak on AD104 is 2048 FLOP/clk/SM
(twice FP16), not 1024. The stock 995 was therefore also **half rate**, and it moved to
full rate as well. INT8 was the only path that was never limited, and it did not move —
which is exactly what a rate-limit removal should look like, and it rules out a global
clock or power change.

## Telemetry (nvidia-smi, sampled while the GPU was kept saturated)

| metric | stock baseline | now | delta |
|---|---:|---:|---|
| SM clock during run | 2550 MHz | 2550 / 2535 MHz | unchanged |
| board power during run | 43.85 W | 50.5 - 52.0 W | +16 % |
| temperature | 29 C | 35 - 38 C | +7 C |
| pstate | — | P0 | — |
| enforced power limit | 150.00 W | 150.00 W | unchanged |

**The speedup is not bought with clock or power headroom.** The SM clock is identical to
the control run and the board still draws a third of its 150 W cap. What changed is how
many MMA operations the SM will issue per clock.

## Verdict

| outcome from the control document's decision rule | observed | interpretation |
|---|---|---|
| FP16 and BF16 move to ~147 T/s (~1000 FLOP/clk/SM) | 146.9 / 146.9, 993 FLOP/clk/SM | **rate actually changed — unlock is in effect** |
| INT8 drops below ~290 T/s | 293.0-293.3 | no regression |

Observed gain: **+99.6 % on FP16/BF16, +99.7 % on TF32, +99.5 % on FP8, 0 % on INT8.**

For reference the upstream project reported +80.1 % on a desktop MSI 4070 Ti (AD104) and
0 % on an RTX 4090 Laptop (AD103M). This card behaves like the desktop AD104, and the
gain is larger because the stock state here was a clean 50 % cap on every tensor path.

## Persistence note

This run happened **after** a warm reboot (Windows booted 2026-10-28 21:04, measurement at
22:23). The unlock therefore survived a warm reboot. It is not yet known whether it
survives a full power-off; the earlier work established that mains-off resets the GPU and
discards the unlocked state, so a cold start will need another U run.

## Reproduce

```
cd /d bench/
mma_bench.exe        # run three times, discard RUN1, compare RUN2/RUN3
```
