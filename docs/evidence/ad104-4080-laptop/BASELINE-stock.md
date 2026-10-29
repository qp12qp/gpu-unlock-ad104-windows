# Stock baseline — RTX 4080 Laptop (10de:27a0), before any unlock attempt

Measured 2026-10-27 02:17 local, Windows boot after the read-only UEFI probe run.
Binary: bench/\mma_bench.exe (unchanged since first build).
Command: mma_bench.exe (default iters=60000 x 8 mma per thread, grid=464 x 256 thr, 58 SM).

Purpose: this is the control arm. The same binary, same arguments, is re-run after
any unlock attempt. The decision rule is written at the bottom.

## Raw wall-clock runs (three consecutive invocations)

| mode | RUN1 ms | RUN1 T/s | RUN2 ms | RUN2 T/s | RUN3 ms | RUN3 T/s |
|---|---:|---:|---:|---:|---:|---:|
| FP16 / FP32 acc | 118.685 | 61.49 | 99.180 | 73.58 | 99.164 | 73.60 |
| BF16 / FP32 acc | 101.614 | 71.82 | 99.178 | 73.59 | 99.132 | 73.62 |
| TF32 / FP32 acc | 101.599 | 35.92 | 99.112 | 36.82 | 99.145 | 36.81 |
| INT8 / INT32 acc | 50.461 | 289.26 | 49.833 | 292.90 | 49.803 | 293.08 |
| FP8 E4M3 / FP32 acc | 99.273 | 147.03 | 99.283 | 147.02 | 99.212 | 147.12 |

RUN1 is the first invocation after idle (clock ramp). RUN2 and RUN3 agree within 0.05 %,
so the stable baseline is RUN2/RUN3. Charge any later comparison against RUN2/RUN3, never RUN1.

## FLOP per SM per clock (stable runs, SM clock sampled 2550 MHz during the runs)

Derived as T/s / 58 SM / 2.55e9. This is the number that matters: it is independent of
clock and of how the benchmark is written.

| mode | measured FLOP/clk/SM | full-rate reference | verdict |
|---|---:|---:|---|
| FP16 / FP32 acc | 498 | 1024 | half rate |
| BF16 / FP32 acc | 498 | 1024 | half rate |
| TF32 / FP32 acc | 249 | 512 | half rate |
| INT8 / INT32 acc | 1982 | 2048 | full rate |
| FP8 E4M3 / FP32 acc | 995 | 1024 | full rate |

## Telemetry during the runs (nvidia-smi, 1 s sampling)

| metric | idle | peak during mma_bench |
|---|---:|---:|
| SM clock | 210 MHz | 2550 MHz |
| board power | 7.06 W | 43.85 W |
| temperature | 25 C | 29 C |
| utilization | 0 % | — |

Peak board power 43.85 W against a 150.00 W enforced limit. **The half rate is not a
power or thermal limit** — this benchmark keeps everything in registers, draws under a
third of the cap, and still lands exactly on 498 FLOP/clk/SM. It is an issue-rate limit.

## Decision rule for the comparison run

Re-run mma_bench.exe with no arguments right after the unlock attempt, after the same
warmup pattern (discard the first invocation, use the second and third).

| outcome | reading | interpretation |
|---|---|---|
| FP16 and BF16 both move to ~147 T/s (~1000 FLOP/clk/SM) | rate actually changed | same as desktop 4070 Ti (+80 %) |
| FP16/BF16 still ~73.6 T/s (~498) | unchanged | same as RTX 4090 Laptop (payload ran, no speedup) |
| INT8 drops below ~290 T/s | regression | stop, revert |

Anything between must be resolved by a third run, not by averaging.

## Reference points from the upstream project

| card | die | unlock result | measured effect |
|---|---|---|---|
| MSI RTX 4070 Ti (10de:2782) desktop | AD104 | completed | FP16/FP32 GEMM 84.08 -> 151.43 TFLOP/s (+80.1 %) |
| RTX 4090 Laptop (10de:2757) | AD103M | completed, getter changed to full | no change at all (90.7 -> 90.7 TF/s) |
| this card (10de:27a0) | AD104M | not attempted | — |
