# Latence.app AD104 Windows changes — 2026-10-29

- Unlock without a keypress: the boot prompt defaults to unlock and waits 1 s (U starts immediately, S/Esc skips, a machine with no keyboard unlocks instead of aborting).
- Remove the blocking boot menu: `{bootmgr}` `timeout 0` plus `displaybootmenu No`. `timeout 0` alone does not hide the "Windows 11 / recovery" menu. `recoveryenabled` and `recoverysequence` are preserved, so WinRE stays reachable via Shift+Restart, Settings and F8. Measured effect: 13 s waiting for a keypress after handoff became 2 s.
- Stop calling `StartImage` on `bootmgfw.efi` on the unlock path. It returns `STATUS_INVALID_IMAGE_FORMAT (0xc000007b)` and bootmgr then reports the image as corrupt. The application reconnects the drivers it detached and returns `EFI_SUCCESS`, letting `{fwbootmgr}` walk on to `{bootmgr}` — the route the containment path already used.
- Fix the final pre-Windows check: an unreachable device (every register reads `0xffffffff`) was treated as a wrong value, so every successful run was marked failed and sent down the containment path. The verdict is now a pure function of `(gone, five register values, fault, verified_at_write)`, covered by four assertions.
- Add `[T]` wall-clock stamps (ENTRY, UNLOCK-DONE, FINAL-CHECK, HANDOFF-ok/fail) to `BOOTTRACE.TXT`, the only channel that outlives `GuBootClose`.
- Second captured board: RTX 4080 Laptop `10de:27a0` (AD104, 58 SM). Stock was a clean 50 % cap on FP16/BF16/TF32/FP8; after the transaction each moved to ~97 % of peak with unchanged SM clock and unchanged INT8. Report: `docs/ad104-4080-laptop-windows.md`.
- Tests: 9335 checks across 48 cases, 17 admission, 90 transaction/port scenarios, `CORE-EXIT=0`; the new verdict branches are covered explicitly.

# Latence.app AD104 Windows changes � 2026-10-24

- Adapt the pinned Ada transaction to checked UEFI PCI I/O, DMA and explicit error/retention state for the captured AD104 profile.
- Add MSI console/GOP ownership capture/validation and callback-based release, exclusive PCI claim and IO/MEM-only restoration.
- Preload a pinned Windows manager from a read-only ESP, require U for one attempt and contain all post-mutation failures.
- Handle periodic USB additions/removals within bounded preflight-classified USB ownership, while preserving GPU/display/console and host ownership invariants.
- Require payload marker, HALT, permission and selector/readout checks before/after one FLR; free DMA only after verified quiescence.
- Record one successful Windows handoff and three numerical/throughput runs; retain exact scope and pending A/B/A/long-duration qualification.
- Publish source-only with an ignored local ESP header, synthetic mock-test GUID, configurable compiler paths, diagnostic source and sanitized evidence. The configured public source rebuilds the historical hardware EFI byte-for-byte; 177 scenarios freshly pass.

Upstream commits and full notices remain in provenance.json and licence files. No Linux target admission changes.
