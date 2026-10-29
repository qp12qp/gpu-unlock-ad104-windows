#!/usr/bin/env python3
"""Static invariants for the AD104 laptop port.

Three properties whose violation costs a physical reboot:
  A. never enumerate the whole firmware handle database - that renumbers
     partitions and breaks bootmgr with 0xc0000225
  B. never hand the firmware an error status - it abandons the boot order and
     drops into a recovery screen with no disk picker
  C. never spin forever - that forces a power cycle, and a power cycle resets
     the GPU, discarding a successful unlock

Comments are stripped before matching so prose cannot produce a false pass or
false failure. Line numbers are preserved.
"""
import re, sys, pathlib

SRC = pathlib.Path(__file__).resolve().parent
files = sorted(SRC.glob("*.c"))

ALLOWED_ENUM = {
    ("app.c", "PciGuid"),          # locate the GPU; the S path uses this too
    ("boot_io.c", "FsGuid"),       # find the Windows volume; S path too
    ("msi_release.c", "GopGuid"),  # fallback only, when console is not our GPU
}
ALLOWED_SPIN = {
    ("checks.c", "GuDisplayPath"), # DevicePath walker: NodeLength()>0 guard, every branch returns
    ("boot_io.c", "GuBootHalt"),    # the halt helper itself; C also asserts it has no caller
}

def strip_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            seg = text[i:j]
            out.append("\n" * seg.count("\n") + " " * (len(seg) - seg.count("\n")))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        else:
            out.append(text[i]); i += 1
    return "".join(out)

def line_of(text, pos):
    return text[:pos].count("\n") + 1

def enclosing_fn(text, pos):
    name = "?"
    for fn in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\{", text):
        if fn.group(1) in ("if", "for", "while", "switch"):
            continue
        if fn.start() < pos:
            name = fn.group(1)
    return name

fails, notes = [], []

# ---- A. enumeration ------------------------------------------------
for p in files:
    code = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
    for m in re.finditer(r"LocateHandleBuffer\s*\(\s*([A-Za-z_]+)\s*,\s*(?:&)?\s*([A-Za-z_]+)?", code):
        kind, guid = m.group(1), (m.group(2) or "")
        ln = line_of(code, m.start())
        if kind == "AllHandles":
            fails.append(f"A: {p.name}:{ln} enumerates AllHandles (volume renumbering -> 0xc0000225)")
        elif (p.name, guid) not in ALLOWED_ENUM:
            fails.append(f"A: {p.name}:{ln} enumerates by unexpected selector {kind}/{guid}")

# ---- B. exit statuses in UefiMain ----------------------------------
app = strip_comments((SRC / "app.c").read_text(encoding="utf-8", errors="replace"))
start = app.index("UefiMain")
body = app[start:]
returns = list(re.finditer(r"return\s+([A-Za-z_][A-Za-z0-9_]*)\s*;", body))
for m in returns:
    if m.group(1) != "EFI_SUCCESS":
        fails.append(f"B: app.c:{line_of(app, start + m.start())} UefiMain returns {m.group(1)}")
notes.append(f"B: {len(returns)} return statements inside UefiMain, all must be EFI_SUCCESS")

# ---- C. logging must survive the USB log being closed ---------------
# After close-USB-log the RUN file is gone; a NOTE that only calls GuLog there is
# silently dropped and a failure while preparing StartImage leaves no evidence.
NOTE_DEF = re.compile(r"#define\s+NOTE\s*\(.*")
for p in files:
    code = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
    for m in NOTE_DEF.finditer(code):
        if "GuTraceLine" not in m.group(0):
            fails.append(f"C {p.name}:{line_of(code, m.start())} NOTE has no BOOTTRACE fallback")
        else:
            notes.append(f"{p.name}:{line_of(code, m.start())} NOTE falls back to BOOTTRACE")

# ---- D. infinite loops and halts -----------------------------------
for p in files:
    code = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
    for m in re.finditer(r"for\s*\(\s*;\s*;\s*\)", code):
        fn = enclosing_fn(code, m.start())
        if (p.name, fn) not in ALLOWED_SPIN:
            fails.append(f"C: {p.name}:{line_of(code, m.start())} infinite loop inside {fn}()")
    for m in re.finditer(r"\bGuBootHalt\s*\(\s*\)", code):
        frag = code[max(0, m.start() - 60):m.start()]
        if re.search(r"VOID\s+$", frag):
            continue  # the definition itself
        fails.append(f"C: {p.name}:{line_of(code, m.start())} calls GuBootHalt() (forces a power cycle)")

print("=" * 62)
print("AD104 laptop port - static invariants")
print("=" * 62)
groups = {g: [f for f in fails if f.startswith(g)] for g in "ABCD"}
print(f"A. no whole-database enumeration .......... {'PASS' if not groups['A'] else 'FAIL'}")
print(f"B. UefiMain never returns an error ........ {'PASS' if not groups['B'] else 'FAIL'}")
print(f"C. NOTE survives closed USB log ........... {'PASS' if not groups['C'] else 'FAIL'}")
print(f"D. no reachable infinite loop ............. {'PASS' if not groups['D'] else 'FAIL'}")
print("-" * 62)
for p in files:
    code = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
    for m in re.finditer(r"LocateHandleBuffer\s*\(\s*([A-Za-z_]+)\s*,\s*(?:&)?\s*([A-Za-z_]+)?", code):
        allowed = m.group(1) != "AllHandles" and (p.name, m.group(2) or "") in ALLOWED_ENUM
        print(f"  {'ok ' if allowed else 'BAD'} {p.name}:{line_of(code,m.start())} "
              f"LocateHandleBuffer({m.group(1)}, {m.group(2) or ''})")
for n in notes:
    print("  " + n)
print("-" * 62)
if fails:
    print(f"{len(fails)} VIOLATION(S):")
    for f in fails:
        print("  " + f)
    sys.exit(1)
print("RESULT: all invariants hold")
