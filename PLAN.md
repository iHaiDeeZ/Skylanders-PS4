# Skylanders: Spyro's Adventure (Wii) → PS4 — port plan

Target: native PS4 package, **60 fps on a base PS4**, no Dolphin.

## What's on the disc (SSPE52, USA Rev 1)

| Item | Finding |
|---|---|
| Code | One `main.dol`, **4.84 MiB of code ≈ 1.27 M PowerPC instructions**, no RELs. Entry 0x80006310. |
| Data/BSS | ~0.75 MiB data, 0.9 MiB BSS. |
| Engine | **Vicarious Visions Alchemy v6.5.0 (`wii-cw-fin`)** + Toys for Bob code (`tfb*` classes). C++, CodeWarrior. |
| Middleware | Havok 5.x physics, Bink video, Activision Portal driver (encrypted USB HID). |
| Nintendo SDK | RVL_SDK Dec 2009 (0x4302_145), statically linked: OS, GX, AX/DSP/AI, DVD, NAND, WPAD/KPAD, **HID** (portal), VI, SC, HBM, plus network libs (SO/SSL/NHTTP/DWC; online parts can be stubbed). |
| FPU use | ~26 k paired-single ops + ~23 k `psq_l`/`psq_st`: the lifter has to handle Gekko paired singles and quantized load/store well. |
| Assets | 4.0 GB: `level/` 3.2 GB (76 levels as `.arc` + `.bld`), `permanent/` 497 MB, `character/` 220 MB. Alchemy IGZ/IGB data stays as-is and is loaded by the game's own code. |
| Symbols | None, but lots of class names (`Gap::Core::...`, `tfb...`, RTTI-style strings) and assert strings. SDK functions can be matched by signature. |

Existing work: **no decomp of this game exists** (only modding tools for the XPEC PC/PS3/360 ports and
for later Alchemy games). Note the XPEC PC/PS3/360 versions are a different codebase from this Toys for Bob Wii build.

## P0 results (2026-11-14)

Outputs in `analysis/` (scripts in `tools/`; dtk project in WSL `~/ssa-build/dtk`, dtk 1.8.4 at `~/tools/dtk-1.8.4`).

- **Entry points: 24,270** (`analysis/entries.txt`): dtk's 19,312 functions + code pointers in data
  + `lis`/`addi` code addresses (Alchemy registers callbacks this way) + prologues after a function end
  + all `bl` targets; 1,236 `lis`/`addi` "addresses" that walk into garbage were dropped.
- **Coverage:** `.text` 99.6% of bytes reached. `.text2` 85%: the unreached ~80 KiB at
  0x804D38E0–0x804E78E0 has no `blr`/prologue at all, so it's a data table in the code section, not code.
- dtk quirk: this DOL has two sections both called `.text`; dtk 1.8.4 writes configs it can't reload.
  `tools/fix-text2-names.py` renames the second to `.text2`. dtk also rejects most extra seeds
  ("unpaired epilogue"), so the entry list is built by `tools/entries.py` instead of by dtk.
- **Hardware access is narrow** (`analysis/mmio-survey.txt`): 83 functions write the GX FIFO
  (always the constant 0xCC008000, never through a pointer variable), 19 touch DSP, 13 VI, 9 PI,
  7 the IPC/Hollywood registers (IOS), 3 AI, 2 DI. Everything else is plain RAM code.
- **Instruction mix** (`analysis/insn-survey.txt`, 1.18 M reached instructions): 5,640 `lwarx`/`stwcx.`
  (Alchemy/Havok atomics), 5,236 `lmw`/`stmw`, ~28.8 k `psq_l/st(x)`, ~13 k paired-single math,
  GQR2–7 rewritten at runtime (quantization can't be a compile-time constant), 524 `mftb`,
  275 `mtmsr`/`mfmsr` (interrupt on/off), BAT/HID writes only in OS init, locked cache barely used
  (1 `dcbz_l`, 3 DMA writes), no string instructions.
- **SDK names:** strings name ~144 SDK functions (DVD, OS, KPAD/WPAD, NAND); GX has none. Not needed
  with the hardware-level approach below. Matching against a compiled SDK would need Nintendo's
  CodeWarrior compilers (not downloaded).

### Decisions from P0
1. **P1 base = Crema's recompiler** (`CEMUPS4`, `Cafe/HW/Espresso/Recompiler`): Espresso ⊇ Broadway,
   paired singles + GQR + `lwarx`/`stwcx.` already verified against the interpreter (`tests/jittest`),
   and `BackendC` already writes whole games as C++ for clang (BotW: 64,667 functions). Needed: a DOL
   loader instead of RPX, Wii memory map (MEM1 0x80000000 + uncached mirror 0xC0000000, MEM2 0x90000000
   + 0xD0000000), FIFO stores to 0xCC008000 emitted as direct appends, other MMIO stores to device handlers.
2. **Renderer at the FIFO level** (like Dolphin's VideoCommon), not GX-API HLE: no GX names needed, the
   83 FIFO writers are found statically, display lists work as on hardware (PI FIFO redirection).
   Hot GX functions can still be replaced natively later for speed.
3. **IOS at the IPC level** (7 functions talk to it): `/dev/di` → files in `/app0`, `/dev/fs` → `/data`
   saves, `/dev/usb/hid` → Portal. Wii Remote: HLE at WPAD/KPAD (names are in strings) instead of
   emulating Bluetooth.

## P1 progress (2026-11-14): the whole game translates to C++ and builds

Harness: `recomp/` = a copy of CEMUPS4's `tests/jittest` (driver, stubs, build/aotbuild scripts) built
against a Skylanders-only copy of Crema's source in WSL `~/ssa-build/crema` (CEMUPS4 untouched).
`recomp/patch-crema-wii.py` turns a fresh copy into the Wii one (rerun after refreshing from Crema):
- **Code at Wii addresses:** MEM1 (0x80000000–0x81800000) allowed beside the Wii U area. The JIT reaches
  the jump table as `[R15 + IP*2 + table]`, so the Wii slots sit 4 GiB above the instance data, in one
  reservation with it (`PPCRecompiler_reserveInstanceData`): emitted x86 unchanged; C++ slot accesses
  through `PPC_REC_JUMP_SLOT`, range checks through `PPCRecompiler_isCodeAddress/Range`.
- **Unsupported instructions leave instead of failing the function** (like `tw`: own segment, LEAVE at
  its address, the interpreter goes on from there). These are the zero words CodeWarrior leaves after
  calls that never return (never run), plus 35 supervisor/system ones (mfmsr/mtmsr, SPRs: HID, BATs, DMA, cache
  ops, FPSCR moves). Opcode 1 (Cemu's HLE instruction) is illegal on Broadway: data, so it leaves too.
- BackendC writes one `case` per entry address (the last segment, as the JIT's table has it).
- `JT_ALL_UNSUPPORTED=1` logs every unsupported instruction.

Steps (WSL, `~/ssa-build/p1`): `tools/dol2bin.py main.dol code.bin`;
`JT_RPX=code.bin JT_FUNCS=entries.txt JT_AOT_OUT=aot jittest`; `recomp/aotbuild.sh aot ssa.aot 12`;
check with `JT_AOT_REAL=ssa.aot`. The generated C++ comes from the game: it stays in `~/ssa-build`.

Results:
- **24,249 entries → 24,249 functions compiled, all written as C++** (81 files, 200 MB C++),
  ~4.6 s to translate. JIT x86: 24.7 MB.
- **`ssa.aot` builds and links** (clang -O2, btver2): 44 MB, 36.4 MiB of code; loader checks pass.
- **Translated vs JIT on real code** (`JT_AOT_REAL`, 2 random states per function): 36,549 runs
  compared, 22 differ, all explained:
  - 15 are self-recursive functions (the harness lets a function's call to itself run on the
    translated side only).
  - 7 are computed branches to garbage addresses (random CTR/LR) that the harness classifies only for
    the Wii U range.
  - Crema's own random-program test is unchanged: the same 361 known kinds and hash `7fefec79144edee8`.
- More data in `.text2` found: Alchemy's XML config text, an embedded `IGA` archive (0x804D2598–0x804E9604)
  and pointer tables. `tools/entries.py` drops the first two; 1,135 possible pointer tables are kept on
  purpose (a missing real function costs more than dead code).

Still to do in P1 (the runtime side):
- A Wii runtime that loads `ssa.aot` with its own jump table (`aot_imports.table` = instance + 0
  layout, or a MEM1-only table biased by 0x80000000 / 4).
- Guest memory: MEM1 at 0x80000000 + uncached mirror 0xC0000000, MEM2 at 0x90000000/0xD0000000.
- GX FIFO stores (0xCC008000) as direct appends; other MMIO to device handlers.
- An interpreter for the leaves: Cemu's, extended with Broadway supervisor instructions.
- mfmsr/mtmsr natively (they sit in every lock).

## Runtime v0 (2026-11-14): boots on the PC to the first IOS request

`recomp/ssa_run.cpp` + `recomp/mmio.cpp`, a mode of the same harness binary (PC host, WSL):

    cd ~/ssa-build/p1 && SSA_RUN=1 SSA_DOL=~/ssa-build/dtk/orig/main.dol SSA_AOT=ssa.aot \
      SSA_DISC="/mnt/e/PS4 Projects/XX/disc/DATA" SSA_SECONDS=10 ~/ssa-build/jittest/jittest

- **Memory:** MEM1 and MEM2 each mapped three times (physical 0x0/0x10000000, cached 0x8/0x9…,
  uncached 0xC/0xD…), which is the Wii's fixed BAT layout. Code run in real mode (`RealMode` → BAT
  setup) reads the same memory.
- **Boot state:** the DOL loaded; low memory as the system menu's IOS + BS2 + IOS56 + the apploader
  leave it (Dolphin's Boot_BS2Emu.cpp/IOS.cpp/VersionInfo.cpp values; FST at 0x817FC480, BI2 at
  0x8179B500); MSR 0x2032, r1 0x816FFFF0.
- **Execution:** `ssa.aot` loaded through Crema's loader (23,311 functions active, every one matching
  the DOL), translated code through the jump table, and in between a Wii interpreter (patch: Cemu's
  interpreter with supervisor mode and no MMU; `rfi`, SRR0/1, BATs, HID work). `.init` is translated
  too (memcpy/memset). The time base is 60.75 MHz from the host clock (`g_ssaTimeBase`; 0 in the tests).
- **Hardware registers:** left unmapped; a fault is emulated by decoding the x86 access
  (mov/movbe/movzx/movsx, 8/16/32 bits, register/immediate), or else by single-stepping the
  instruction over a scratch page (any other form, flags exact). Each register's first access is
  logged (`SSA_MMIO_LOG=1`: every access).
- **Devices so far:**
  - EXI: transfers complete at once; the RTC/SRAM chip on channel 0 device 1 (RTC, SRAM read by DMA).
  - DSP: reset self-clears, ARAM DMA completes with ARINT, the init program's mail 0x80544348
    (`__OSInitAudioSystem` handshake).
  - Write-1-to-clear status bits (EXI CPR, DSP CSR).
  - Everything else: a stored register file.

How far it gets:
`__start` → `__init_data` → `OSInit`: interrupts, MI, DSP/AI, EXI probe (`EXIGetID`), SRAM, SI, VI,
`__OSInitAudioSystem`, `BATConfig` (real mode), Hollywood GPIO/PLL → **first IPC request to IOS**
(0xCD000000 ← 0x135E0020). The main thread then sleeps, and the scheduler idles in `SelectThread`
waiting for an interrupt.

Next (runtime v1):
1. **Interrupts:**
   - PI INTSR/INTMR (0xCC003000/04) and Hollywood's PPC IRQ flag/mask (0xCD000030/34, IPC = bit 30).
   - When MSR.EE is set: the external interrupt at 0x80000500 (SRR0/SRR1), delivered at dispatcher
     boundaries.
   - The decrementer (0x80000900) for OS alarms.
2. **IPC + IOS HLE** at the IPC boundary (Dolphin's IOS HLE as the reference):
   - The protocol: PPCMSG/PPCCTRL/ARMMSG, ack + reply bits, the reply interrupt.
   - Devices: `/dev/es`, `/dev/fs` (NAND in a host folder: SYSCONF, saves), `/dev/stm/*`.
   - `/dev/di`: disc reads by partition offset, served from the extracted files through the FST and
     boot.bin's offsets.
   - `/dev/usb/hid` (the Portal), `/dev/usb/oh1` (Wii Remote; or WPAD HLE).
3. Then VI (retrace interrupt), the GX FIFO, and moving the runtime onto the PS4 (ps4-kit).

## Runtime v1 (2026-11-14): through OSInit into the game's main, to the Wii Remote stack

New: `recomp/ios.cpp` (IPC + IOS), `recomp/disc.cpp` (the disc), `tools/make-nand.py` (NAND),
`tools/addr2name.py` / `tools/xref.py` / `tools/ppcdis.py` (was dis.py: the name hid Python's own dis module) (diagnosis). Run with
`SSA_NAND="/mnt/e/PS4 Projects/XX/nand"` too (make it once: `python3 tools/make-nand.py nand`).

- **Exceptions:** the external interrupt (0x500) and the decrementer (0x900), taken between dispatcher
  runs when MSR.EE is set. The vector code runs in the interpreter in real mode at the physical
  vector address.
- **The decrementer:** belongs to the runtime (hooks in the interpreter). It counts at the time-base
  rate, and its exception fires once when it passes zero.
- **PI cause** is computed from the device lines: EXI, DSP, PE token/finish, VI, IPC, and the reset
  button (not pressed).
- **IPC** follows Dolphin's state machine: X1/Y1/Y2/IY1/IY2, ack then reply, the Hollywood IRQ
  flag/mask.
- **IOS (HLE):**
  - Opening the known `/dev/*` devices succeeds. Files come from the host NAND (SYSCONF, setting.txt;
    missing: -106).
  - ES: title ID, title directory, ticket view, consumption.
  - STM's event hook stays pending.
  - `/dev/di`: reads from the extracted files through the FST, a rebuilt raw disc
    (header/partition table/region/partition header) and OpenPartition. Retail drive behaviour for
    the **copy-protection checks**: an unencrypted read past the single-layer end (0x118280000,
    "error #001" check) gives a drive error with 0x052100, and `ReportKey` gives 0x052000.
  - Bluetooth dongle event/data reads stay pending.
- **Low memory:** also setting.txt at 0x3800, and the apploader's IOS version word at 0x3188.
  Without it, OSInit stops with ERROR #002.
- **VI:** the display interrupts DI0–DI3 fire at their lines (NTSC, 59.94 Hz).
- **GX:** BP writes are picked out of the FIFO stream: `PE_DONE` raises the finish interrupt and
  `PE_TOKEN(_INT)` the token one. `GXDrawDone` returns, though nothing is drawn yet.
- **Interpreter fixes (patch):**
  - The supervisor SPR getter lacked SRR0/SRR1 (every exception looked unrecoverable to the OS),
    DMAU/DMAL and DEC.
  - Reading PVR gives Broadway's ID.
- **MMIO:** the single-step fallback now passes on writes of an unchanged value. clang turns
  `lhz/ori/sth` into `or word [mem], imm`, and the PE finish acknowledge was lost that way: an
  interrupt storm.
- **Diagnosis:**
  - `SSA_WATCH=<pc>` prints the registers and the strings r3–r6 point to; `SSA_DUMP=<addr,...>`
    dumps memory.
  - At the end: a thread dump (state, wait queue, stack of return addresses: pipe it through
    `tools/addr2name.py`) and the external interrupts by PI cause.

How far it gets (30 s on the PC):
1. OSInit finishes:
   - IPC/IOS, SYSCONF, setting.txt and ES;
   - the disc checks pass (error #001 and ReportKey);
   - the ticket view, `play_rec.dat` (missing) and the network config (NCD/KD).
2. Then the game's `main`: GX init (the CP FIFO at 0x0175D480, the PE), the first frame's commands,
   and `GXDrawDone`.
3. **The Wii Remote stack (BTE/WPAD):** it opens the Bluetooth dongle and sends its first HCI
   command. The main thread then waits for the remote stack in a loop under game code
   (`fn_80161DE4` → `fn_8038F8C0` → …).

Next:
1. **The Wii Remote.** Two routes:
   - (a) Emulate the Bluetooth dongle + a Wii Remote, as Dolphin does (HCI events,
     L2CAP, HID reports), fed from the DS4. The game's own WPAD/BTE code runs unchanged.
   - (b) HLE at WPAD/KPAD: replace those functions natively. Less code to emulate, but each function
     has to be found by hand (no symbols) and kept in step with the library's internal state.
2. Then: the disc reads the game starts doing, the GX FIFO (the renderer, P3), audio (DSP AX HLE),
   the Portal (`/dev/usb/hid`), and moving the runtime onto the PS4.

## Runtime v2 (2026-11-14): the Wii Remote over emulated Bluetooth

`recomp/bt.cpp`: the Bluetooth dongle (`/dev/usb/oh1/57e/305`) and a Wii Remote with a Nunchuk,
entirely in software. Behaviour after Dolphin's BTEmu.cpp / WiimoteDevice.cpp / WiimoteHIDAttr.cpp /
WiimoteEmu (**GPL-2.0-or-later**; the runtime takes that licence). The game's own BTE/WPAD stack runs
unchanged on top of it.
- **USB V0 requests:** an HCI command on the control endpoint; event (0x81) and ACL-in (0x82) buffers
  held until there is something to fill them with (`iosReplyLater`); ACL-out (0x02) carries L2CAP to
  the remote.
- **HCI:** the controller's answers. The remote connects itself once page scan is on: Connection
  Request → Accept → Role Change + Connection Complete (handle 0x100). Also auth, remote
  name/version/features, clock offset, packet type, sniff mode, completed-packet counts. The link
  key is returned by Read Stored Link Key.
- **L2CAP:** the remote opens HID control (0x11), then interrupt (0x13), both configured with MTU 185.
  SDP answers with Dolphin's records.
- **The Wii Remote:**
  - EEPROM with Dolphin's IR/accel calibration.
  - Output reports: LEDs, mode, IR, speaker (data ignored for now), status, memory write/read
    (EEPROM, speaker 0xA2, extension 0xA4, IR 0xB0).
  - Acks.
  - Data reports 0x30–0x37/0x3D, at 200 Hz in continuous mode.
  - A Nunchuk (ID 00 00 A4 20 00 00, calibration at 0xA40020/30). Encrypted extension data (old
    init) is not emulated yet; it's logged if a game asks for it.
- **Input:** `bt_input` (buttons, accel, Nunchuk stick/C/Z), neutral for now. It is where the binding
  layer will feed the DS4.
- **SYSCONF `BT.DINF`:** the emulated remote is registered (`tools/make-nand.py`).
- **Also fixed:**
  - The interpreter's supervisor `setDEC` was an empty stub (patch), so OS alarms never fired. The
    Bluetooth stack runs off them.
  - The AI sample counter (0xCD006C08) counts at 48 kHz while playback is on, and resets with
    SCRESET. The AI start-up waits for it to move.

How far it gets (40 s on the PC):
1. The BTE stack initializes; the remote connects; both HID channels open.
2. WPAD reads the EEPROM/calibration, sets continuous mode 0x30, the LEDs and IR, and receives
   button reports.
3. The main thread gets past the AI start-up into the **DSP mailbox boot**: it waits for the DSP's
   first mail, then sends 0x80F3A001 (handing over the audio microcode).

Next:
1. **DSP / AX audio (HLE):** the ROM boot mail handshake, the microcode handover, then AX
   (Dolphin's DSPHLE UCode_AX as the reference), with mixing out to the host/PS4.
2. Then the disc reads the game starts doing, the GX renderer (P3), and the PS4 runtime.

## Runtime v3 (2026-11-14): audio DSP (HLE), disc reads, and the first game data

`recomp/dsp.cpp`, after Dolphin's DSPHLE (DSPHLE/MailHandler/ROM/INIT/AX/AXWii; GPL-2.0-or-later):
- **Mailboxes:** CPU→DSP mail goes to the active microcode and the busy bit clears at once. DSP→CPU
  mail is queued, hidden while the DSP is halted, taken by reading the low half, and can carry the
  DSP interrupt (CSR bit 7).
- **CSR:** reset → ROM; DSPInit 1→0 → INIT (0x80544348, with DSPInitCode busy for ~130 ticks).
- **ROM boot:** 0x8071FEED and the boot parameters; the Ector hash picks AXWii. The game's microcode
  hash is **0x4CC52064**, the same AXWii build as Bleach: Versus Crusade in Dolphin's list (new command
  set).
- **AXWii:** mails DSP_INIT. Each frame the command list (0xBABE0000|size, address) is walked to
  CMD_END (command lengths as Dolphin's), then DSP_YIELD with a delayed interrupt. Task mails
  (resume, new microcode, reset, continue) are handled. **v1 doesn't process voices or mix yet:
  the output is silence.**
- **Audio DMA (0x5030–0x503A):** a started buffer plays out at 32 kHz in real time, then AIDINT, then
  the next buffer starts. That's what drives the game's audio frames: about 300 AX frames a second,
  in real time.
- **AI:** the sample counter (0xCD006C08) counts at 48 kHz while playback is on.

Other fixes found on the way:
- The interpreter's supervisor `setDEC` was an empty stub (patch), so OS alarms never fired.
- `mftb` in the Wii interpreter now reads the runtime's time base (patch: `getTB`).
- DICFG reads 1 (Dolphin's power-on value).
- **The data partition is open at boot**, as the apploader leaves it (Dolphin BS2: ChangePartition).
  The game's first DVDLowRead failed without it and ended in the disc library's fatal error.
- **IOS fds are the lowest free number (≤ 32).** Ever-growing fds made SDK code index per-fd tables
  out of range (a crash through an ASCII "pointer").
- NCD GetLinkStatus answers LINK_NONE (no network), and unhandled ioctls zero their outputs.
- **Disc reads take time:** a serial drive queue, 0.2 ms + size at 8 MB/s, data written when the read
  completes, as on hardware.

Diagnosis added:
- `SSA_INTERP=1`: interpreter only.
- `SSA_TRACE=a,b,...`: logs entries to the listed functions. With translated code it unlinks their
  slots and turns off direct calls, so they're interpreted and visible.
- `SSA_WWATCH=addr`: logs writes to a guest word, with the host code that did them
  (`ps4_crash_jit_info`).
- A guest stack and registers on faults (MEM1 and MEM2 stacks).
- `tools/fst-lookup.py` (disc offset → file), `tools/callers.py` (bl callers), `tools/xref.py`.

How far it gets (on the PC):
1. Everything up to the Wii Remote, then the DSP boots AXWii and audio frames run.
2. DVDInit's checks pass, and the game reads the HOME Button menu resources (`files/hbm/*`),
   `saveBanner/saveIcon.tpl` and all of **`permanent/global.bld`** (3.8 MB, an Alchemy `IGA` archive,
   little-endian, zlib-looking blocks), then starts on `permanent/global.arc` (516 MB).
3. It also opens `/dev/usb/hid` (the Portal).
4. **Then it crashes in a resource loader:** `fn_8011F614`, a reference-fixing loop. It decodes
   section references from a packed stream (`fn_8011E0A8`), resolves them (`fn_80120310`), and replaces
   each stored index with a table entry: `lwzx r4, r27, index*4`. Some index is garbage
   (0x017F0000-like), and the faulting address changes between runs.
   - **Ruled out:** translated code in the loop itself and in all of 0x80110000–0x80130000 (678
     functions interpreted: still crashes); `memcpy`/`memset`; zlib's `inflate` at 0x801AA320 (never
     called for this); wrong disc data (FST mapping checked: the reads are the right files); instant
     disc reads (now timed: still crashes).
   - **Seen:** the word first watched was written by `memcpy` (a valid index, 7) and then fixed up
     correctly; the crash moves to other words when timing changes.
   - **Solved in runtime v4 (below):** threads shared the FPU registers.

## Runtime v4 (2026-11-14): FPU context switching, HID2, the GX FIFO breakpoint

**The resource-loader crash was memory corruption from shared FPU registers.** Snapshots of two runs
at the loop's start (`SSA_SNAP`) differed at scattered spots of the loaded data. The differing words
were other threads' float values (single-precision results in double format, the int→double constant
0x43300000_80000000), and later runs of zeros. The cause:
- The OS switches FPU registers lazily. A context switch clears MSR.FP unless the new context owns
  the FPU, and the first FP instruction traps (0x800, FP unavailable) to `OSSwitchFPUContext`, which
  saves the owner's FPRs into its context and loads the current context's. The runtime never raised
  0x800, so all threads (and interrupt callbacks) shared one set of FPRs. The game copies memory with
  `lfd`/`stfd`; an interrupted copy wrote another thread's floats into the data.
- **Fix (`fpuSwitch` in ssa_run.cpp):** the runtime does that switch itself as soon as code runs with
  translation on and FP off: save the owner's FPRs, FPSCR and paired-single halves into its context
  (state bit 0 set), load the current context's (if it was ever saved), make it the owner, set MSR.FP
  and the context's SRR1.FP. It skips the trap's GPR save into the current context. That makes it
  safe anywhere, also in interrupt handlers before they switch to their own context: taking a real
  0x800 there would overwrite the interrupted thread's saved registers. A first try that raised 0x800
  eagerly only with interrupts on still left zeros from interrupt callbacks. Interpreting
  instruction by instruction while interrupts were off fixed it, but ran 860 M interpreted
  instructions in 90 s.
- **HID2 read as 0** in Crema's interpreter, so the OS's FPU save skipped the paired singles' second
  halves (`HID2.PSE`). It's now kept (`g_wiiHID2`, power-on 0xE0000000), patch in
  `patch-crema-wii.py`.

**The next stop was the GX FIFO breakpoint.** The main loop keeps a 6-slot frame ring and sleeps
while more than one slot is in use. Slots are freed by the VI post-retrace callback and by GX's
breakpoint callback, which runs from the command processor's (CP) interrupt. After a frame the game
reads the FIFO write pointer, puts the CP breakpoint there and enables the breakpoint interrupt. With
the GPU caught up, that fires at once. **Fix (mmio.cpp, after Dolphin's CommandProcessor/GPFifo):**
- write-gather bytes go to memory in 32-byte bursts at PI's FIFO write pointer (base/end/wrap);
- in linked mode the CP's write pointer follows;
- the "GPU" consumes at once: the read pointer follows the write pointer, but an enabled breakpoint
  stops it;
- CP status reports the breakpoint (bit 4) and idle (bits 2/3), and control bit 5 turns a breakpoint
  into the CP interrupt (PI cause 0x800).

After both fixes the runs are **deterministic**. The boot loads `global.bld`, `level/Title.bld` and
part of `level/Title.arc`, and both ring callbacks run.

**Then a memory pool ran out: the runtime now runs the disc's apploader.** The loader thread
crashed in `fn_800F6C54` on a NULL object. A NULL read lands on physical 0, which holds the disc ID
`SSPE`, and that got used as a vtable. The object was memory pool 0x1C (class vtable 0x80493A08:
+0x10 base, +0x14 size), and it had no memory:
- At startup (`fn_804D0D08`) the game builds MEM1's big heap: the free arena, minus a 0x40000
  reserve (config key `Gfx/@gfxFifoSize`), minus 0xC00.
- It then carves six pools out of it. Their sizes are constants (base + extra, from the table
  built in `fn_80542230`).
- The last one (0x79C00) was ~0x27BEC short, because the arena's top was ~390 KB too low.
- `__start` sets the arena top right after the argument block in BI2 (BI2+8 → BI2+0x1FC0:
  `tfbGame_gold.elf -build us_20120207_154242`).
- The runtime had BI2 at 0x8179B500, Dolphin's value *before* its apploader runs. The real
  apploader puts the FST at the top of MEM1 and BI2 0x2000 below it, and sets arena high and
  0x80003110 to the FST.
- **Fix (`runApploader` in ssa_run.cpp, after Dolphin's CBoot::RunApploader):** the apploader
  image runs at 0x81200000 in the interpreter: entry, init (OSReport = a `blr` at 0x81300000),
  main, with one partition read per call until it returns 0, then close. That gives FST
  0x817FD820, BI2 0x817FB820, arena high 0x817FD820. It runs after the fault handlers are
  installed, since it reads a hardware register (0xCD006430).

**Where it is now:** 4 minutes on the PC without a fault. The loader thread finishes and exits, and
the main loop runs: VI, CP-breakpoint and audio interrupts, about 30 M hardware accesses (GX FIFO
writes). It streams level data: `level/Title`, `Challenge_Level_008/009/010`, `Level_001_Castle`,
`Level_002_Dungeon`. That's probably the title screen with its background or attract levels. Nothing
is drawn yet (no renderer), and most of the run is still in the interpreter (442 M interpreted
instructions in 240 s), which is a performance job for later.

Diagnosis added:
- `SSA_DUMP` takes `rN+off` (register-relative) and shows the strings that dumped words point to.
- `SSA_SNAP=prefix` (with `SSA_WATCH`): MEM1/MEM2 to files at the watch point, to diff runs.
- `SSA_WWATCH=addr,len`: counts writes to a range by host code (with the translated function).
- `SSA_IPROF=1`: interpreted instructions by 256-byte block, with MSR.EE/FP.
- `SSA_INTERP_AFTER=s`: translated code until then, interpreter after. `SSA_TRACE` only sees calls
  that come back to the dispatcher; calls inside one generated C++ file are plain C calls.
- Faults name the translated function the host code was in.
- `tools/disp-users.py` (loads/stores with a given field offset), `tools/ppcdis.py` (was `dis.py`,
  which hid Python's own `dis` module).

## Renderer R0 (2026-11-14): the GX command stream decoded, with a preview

The plan's earlier idea was to HLE at the GX API level. Now that the FIFO stream is there and
complete (CP model, runtime v4), the renderer works at the **FIFO level**, as Dolphin does: one
decoder for everything the game draws, display lists included, with no per-GX-function hooks.

`recomp/gx.cpp` (after Dolphin's VideoCommon: OpcodeDecoding, VertexLoader, XF/BP/CP state; GPL):
- Fed with every 32-byte burst the gather pipe writes (`gatherBurst` → `gxFifoData`). It keeps the
  bytes that don't yet form a whole command.
- Commands: NOP, LOAD_CP_REG, LOAD_XF_REG, LOAD_INDX_A–D (indexed XF loads from arrays 12–15),
  CALL_DL (decoded from memory where it's called), INVALIDATE_VTX_CACHE, LOAD_BP_REG (with BP
  0xFE masks), and draws (primitive + vertex format + count + vertices).
- Vertex formats: VCD_LO/HI and VAT A/B/C, so every vertex's size and layout, with direct and
  indexed attributes (CP 0xA0+/0xB0+ array base/stride): position/normal (NBT, index3), two
  colors (all 6 formats), 8 texture coordinates, and the position/texture matrix indices.
- Transform: the position matrix (PNMTXIDX or MATINDEX_A) from XF memory, projection (XF
  0x1020–0x1026, perspective/orthographic), viewport (XF 0x101A–0x101F, EFB origin 342).
- A frame ends at the EFB → XFB copy (BP 0x52 bit 14); bit 11 clears the EFB.
- **Preview backend (PC only):** a software rasterizer into a 640×528 EFB. Each draw gets its own
  color (no textures yet), times the vertex color, flat-shaded. It honors the depth mode (BP 0x40)
  and outlines blended draws (BP 0x41). `SSA_FRAMES=dir` writes PNGs every `SSA_FRAMES_EVERY`
  frames; `SSA_GX_LOG=1` logs per-frame counts.

Results (200 s on the PC):
- 15,900 XFB copies and 7.7 M commands, **0 unknown opcodes**. Every other copy is empty: two
  copies per frame.
- The boot shows a 1-quad screen (fade/loading), then 5–6 quads, then a 538-triangle frame. It
  settles on a steady screen of 80 draws / 883 triangles / 17 display lists.
- `preview/r0-frame*.png`: the steady screen is a 2D layout (a segmented bar near the bottom,
  boxes, a panel on the right) over a radial burst of quads (light rays). The positions and the
  orthographic projection come out right.

**R1 (2026-11-15): textures in the preview.** `gx.cpp` now has:
- **Textures:** the BP texture registers (IMAGE0 size/format, IMAGE3 address, MODE0 wrap) and
  every GX format (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4/C8/C14X2 with TLUTs, CMPR) decoded
  from their tiles. TLUT loads (BP 0x64/0x65) go into a 1 MB TMEM model.
- **Coordinates:** texgens (XF 0x1040+: regular, from position or texture coordinates, ST/STQ)
  through the texture matrices (in the vertex or MATINDEX_A/B).
- **Color:** color channel 0's material color, from the vertex or XF 0x100C (no lighting).
- **Shading:** TEV stage 0 as texture × color, with blending (BP 0x41 factors), alpha test
  (rough) and the clear color.
- **Speed:** only captured frames are drawn, and decoded textures are cached per captured frame.

`preview/r1-*.png`, in boot order: the **Wii Remote strap screen**, the **legal screen** over the
sky, and the **title screen** ("Press Ⓐ to begin"). They come out right. Not yet right: the logo
(a white silhouette) and the two side glows (white panels) need the other TEV stages, konst
colors and the TEV's own blending. The game sits on the title waiting for Ⓐ: the emulated Wii
Remote sends neutral input.

**Past the title (2026-11-15): input, the Portal, the NAND.**
- **Scripted input:** `SSA_PRESS="seconds:BUTTON,..."` (bt.cpp) holds Wii Remote/Nunchuk buttons for
  a quarter second each: A B 1 2 PLUS MINUS HOME UP DOWN LEFT RIGHT C Z. It's a test switch until the
  DS4 binding layer feeds `bt_input`.
- **The Portal of Power, emulated (`recomp/portal.cpp`, after Dolphin's USB_HIDv4, SkylanderUSB/
  Portal, SkylanderFigure/Crypto; GPL):**
  - Behind `/dev/usb/hid`: version 0x40001; the device-change list carries the portal's
    descriptors (VID 0x1430, PID 0x0150, HID, endpoints 0x81/0x02).
  - Control messages carry the commands: R(eady), A(ctivate), S(tatus), Q(uery block),
    W(rite block), C/J/L (lights), M, V. A control message's reply count is length + 8.
  - Interrupt reads get the queued answer after ~22 ms, or a status packet after ~2 ms.
  - Before this, the game asked for the HID version, got 0 and shut HID down: "Can't find the USB
    Wireless Receiver".
  - Figures: `SSA_PORTAL="path[=id[:variant]],..."` loads 1 KB `.sky` dumps. A missing file
    becomes a new figure of that character, as Dolphin's portal makes them (random NUID, id/variant
    with their CRC16, CRC48-derived sector keys). The game's writes are saved back.
    Example: `figures/spyro.sky=16`.
- **NAND filesystem (`ios.cpp`, after Dolphin's FS device):** `/dev/fs` CreateFile/CreateDirectory,
  Get/SetAttribute, Delete, Rename, GetStats, ReadDirectory, GetUsage, plus GetFileStats on files.
  Before this nothing answered, the save couldn't be created, and the game said "not enough
  available space in Wii System Memory". Now it writes `Save_Header`, `banner.bin` and
  `Save_Slot_1..4` under `title/00010000/53535045/data`. Test runs use a fresh copy of `XX/nand`.

- **The locked cache:** 16 KB of on-chip scratch at 0xE0000000. The game loads and drains it by
  DMA (DMAU/DMAL, which Crema's interpreter already does) and uses it with plain loads and stores.
  Level loading crashed on it until the runtime mapped those 16 KB.

**The first level runs (2026-11-15):** after the movie and the "Loading…" card, the game plays
the opening level. It draws ~35,000 draws / ~360,000 triangles / ~105 textures per frame, ran
several minutes without a fault, and shows Flynn's dialogue ("Wowsers! Look at all these Balloons
leaving…", `preview/r3-1-first-level-dialogue.png`). In the preview the 3D scene is black: it's
drawn, copied from the EFB into a texture (post effects) and cleared, and the preview has no EFB
copies to textures. It now writes the EFB before such a copy as `frameN-sceneK.png`:
`preview/r3-2-first-level-3d-scene.png` shows the sky, clouds and a floating island, with near
geometry blown up (the preview has no near-plane clipping, lighting or full TEV), and
`r3-3-…-shadow-pass.png` a grey pass, probably shadows. A 760 s run ended without a fault, in the
level the whole time from ~10 minutes in.

The boot now goes: strap → legal → title → (A) autosave notice → CHOOSE PLAY ("Skyland
Adventures") → save slots ("NEW") → new game → the intro movie → loading → the first level. The movie shows as grey stripes:
Bink's YUV planes are combined in the TEV. See `preview/r2-*.png`.

## Renderer R2 (2026-11-15): the Vulkan backend

The software preview is gone; the decoder now feeds a Vulkan backend (`recomp/gx_vk.cpp`, shaders in
`recomp/shaders/`, compiled to SPIR-V headers by `build.sh`). On the PC it runs on Mesa's lavapipe
in WSL (the only Vulkan device there; same Mesa 26.0.8 as the kit's RADV). Headers and glslang come
from Ubuntu's packages unpacked without root into `~/ssa-build/vkdeps` (`apt-get download` +
`dpkg -x`).

Split of the work:
- **CPU (gx.cpp), the transform stage** (moved to the GPU since, see below; `SSA_GX_CPUXF=1`): positions through the position matrix and projection
  into Vulkan clip space (viewport less the scissor offset BP 0x59, depth in 2^24 units), normals
  and binormals (all formats, NBT, index3) through the normal matrix, **GX lighting** for both
  color channels, color and alpha (material/ambient from the vertex or registers, 8 lights,
  spec/spot attenuation, diffuse sign/clamp), and **all 8 texgens** (sources: position, normal,
  binormals, colors, texture coordinates; ST/STQ; AB11/ABC1; texture matrices; post-transform
  matrices with normalize when dual-tex is on; color texgens; emboss copies its source for now).
  Quads, strips and fans become indexed triangle lists.
- **GPU:** clipping (the old preview's giant near polygons are gone), rasterizing, and the **TEV as
  an uber-shader** (`tev.frag`, after Dolphin's UberShaderPixel): up to 16 stages, the integer
  combiner math (bias/scale/clamp, rounding), the 8 compare modes, swap tables, konst colors,
  register state, all 8 texture maps (SU texture-coordinate scale, wrap and filter samplers), the
  full alpha test, and fog (no range adjustment yet). Blending, logic ops, depth test/update,
  culling, scissor and color/alpha update masks are pipeline state, made on demand.
- **Data flow:** vertices are pulled from a storage buffer (clip position, two RGBA8 colors, s/t/q
  per texgen); draw state goes to a storage buffer of 512-byte states (deduplicated); textures are
  one descriptor array (8192 slots, 36 samplers). Consecutive draws with the same pipeline,
  scissor and state are merged into one GPU draw.
- **Textures:** decoded once and cached by address/format/TLUT, revalidated once a frame by a hash
  of their RAM (and palette), evicted after 600 unused frames.
- **EFB copies:** the copy rectangle is rendered (box-filtered for half size) into a texture kept
  for that RAM address, in the layout the matching texture format decodes to (I8 from R8, IA8
  from RG8, Z formats from depth, the intensity/YUV conversion). A later texture at that address
  and size uses the GPU copy while the RAM there is unchanged. The clear bit clears the rectangle
  with the clear color/depth, as the update masks allow.
- **XFB copies** end the frame: one frame in flight, submit and wait; captured frames are read back
  to PNGs. On the PS4 this becomes the present (kit WSI).

Findings on the way:
- GX's front faces are **counter-clockwise** in this clip-space mapping (`SSA_VK_FRONT=cw` flips it).
  With clockwise, the full-screen quads (GX "cull front") were culled and every frame was black.
- The destination-alpha constant (BP 0x42) replaces only the alpha written to the EFB, not the one
  blending uses (Dolphin uses dual-source blending for it). Using it as the shader's alpha made
  every blended 2D draw invisible. Without dual-source blending it now applies only to draws that
  don't blend.

Switches: `SSA_VK_ALL=1` draws every frame (default on the PC: only the captured frame and the two
before it); `SSA_VK_TRACE=n` logs the first n draws' state; `SSA_VK_DEBUG=1/2/3` shows map 0's
texture / color 0 / texture coordinate 0 instead of the TEV.

`preview/r4-*.png`: the strap screen and the title screen with the full TEV (the logo in color; the
R1 preview could only draw a white silhouette), the loading card, and the **first level in 3D**
behind Flynn's dialogue: sky, floating islands and rocks drawn and post-processed through EFB
copies (the R1 preview showed black there). A 640 s run: 481k draws merged into 18k GPU draws
(~26x), 38 pipelines, 355 textures, 416 EFB copies sampled 58k times, no mid-frame flushes.

**The spikes in the level (fixed 2026-11-15): Wii GQRs were dropped.** The island's buildings drew
as hard-edged shards (`preview/r5-0-before-spikes.png`). Their vertex arrays (interleaved s16
positions, e.g. at 0x10C02820) held 32-bit floats, even ±infinity (`7F800000`/`FF800000`), when
they were drawn. The CPU fills those arrays with paired-single quantized stores (`psq_st`), and
the format comes from the GQRs. Crema is a Wii U CPU: `psq_l`/`psq_st` read the Wii U's user-mode
copies UGQR0–7 (SPR 896–903). Broadway's GQR0–7 are SPR 912–919, which nothing handled. So every
`mtspr GQRn` (the game's set-up, the OS's per-thread restore) was dropped, every quantized load
and store ran as a plain float, and `mfspr` read 0. Fix (`patch-crema-wii.py`, GQR_REMAP): 912–919
→ 896–903 in the interpreter's and the recompiler's mtspr/mfspr. The AOT code needs no rebuild:
it leaves to the interpreter at those instructions. The fix touches all CPU code that uses
quantized loads and stores (animation, physics, particles), not only rendering. Now the opening
scene is right: Flynn's balloon, the town and the floating islands
(`r5-1-first-level-gqr-fix.png`, `r5-2-island-scene-pass.png`), and a 680 s run had no fault.

How it was found, with tools that stay:
- **GX recorder and replayer** (after Dolphin's FIFO player): `SSA_GXREC=file` records
  `SSA_GXREC_FRAMES` frames (default 3) from the first frame with `SSA_GXREC_DRAWS` draws (default
  20000). It stores the GX state and all of RAM, then the FIFO bytes, each preceded by the RAM
  ranges the decoder read that changed since. `gx_replay <file> <dir> [VAR=value...]`
  (built by `build.sh link`) draws them again in about 1 s instead of a 10-minute run to the level.
- Replay switches: `SSA_VK_DUMP_COPIES=dir` writes every EFB copy (each render pass) as a PNG;
  `SSA_GX_ONLY=lo-hi` / `SSA_GX_SKIP=lo-hi` draw only / leave out a frame's draws in that range;
  `SSA_GX_INFO=lo-hi` logs those draws' vertex formats, array bases and first positions;
  `SSA_VK_SKIP_KEY=mask=value` leaves out draws by pipeline key (e.g. `1=1`: blended ones).
- Ruled out on the way: per-vertex and XF/CP MATINDEX (`SSA_GX_MATINDEX=xf` stays as a switch),
  skipped/unmapped indices, alpha, and GPU timing.
- **The decoder now gets what the GPU reads, when it reads it:** the blocks the CP read pointer
  passes, from the FIFO in RAM (mmio.cpp `cpUpdate`), not the gather pipe's bursts. It's the same
  in linked mode, and right for games that build commands in a FIFO the GPU isn't attached to.
  `SSA_GX_DEFER=1` (decode at draw-done/token sync points) stays as an experiment switch, off.

Missing: mipmaps and LOD bias, indirect textures (water/heat effects), Z textures, the bump alpha,
lines and points, dual-source blending for the destination alpha, EFB reads/pokes and copies to
RAM for the CPU, fog range adjustment, and presenting on the PS4.

**The transform on the GPU (2026-11-15).** `tev.vert` now does the whole XF stage: position and
normal matrices (per-vertex or MATINDEX), projection and the clip-space mapping, GX lighting with
its rounding, all 8 texgens with post-transform matrices. It is gx.cpp's `transform()` ported line
for line. The CPU only **fetches**: each vertex is decoded to floats in a variable layout (word 0:
the XF snapshot index + a layout mask, then position, [2 words of 6-bit matrix indices], [normal or
NBT], colors 0/1 as RGBA8, [s/t for each texture coordinate the format has]). XF state goes to a
ring of **XF snapshots** (868 words: XF 0x000–0x0FF, 0x400–0x47F, 0x500–0x5FF, 0x600–0x67F,
registers 0x1000–0x105F with the MATINDEX in use, the scissor offset), a new one only after an
XF/MATINDEX/BP 0x59 change. Because each vertex names its snapshot and indices are word offsets
(no per-draw stride or matrix in the push constants), draw merging is unchanged.
- Level recording (3 heavy frames, ~310k vertices and ~23k draws each): 2531 GPU draws as before,
  **3132 XF snapshots for 69k draws**. Frames match the CPU transform within 5/255 (frame 2 exact).
- CPU time in the vertex loops per heavy frame (this PC, quiet): CPU transform ~25 ms → reference
  fetch ~8 ms → **planned fetch ~5 ms** (`fetchDraw`: arrays resolved once per draw, decoders per
  component format, no per-component division; ~16 ns/vertex, not cache-miss bound). Bit-identical
  frames to the reference fetch. The replay's wall time goes up on lavapipe (it runs vertex
  shaders on the CPU); on RADV that work is the GPU's.
- Fixed on the way: with three normal indices (NBT index3), element k is at its index plus k × 3
  components (as Dolphin); it read element 0 for all three.
- Live 680 s run (r6): no fault, 314k draws → 13k GPU draws, 17.9k XF snapshots. Its captures hit
  only a 34-draw state of Flynn's first dialogue with a black background (the pre-change run cp1
  had the same 34-draw frame; when the level comes varies per run). Likely the game showing an
  earlier EFB copy of the scene, made in a frame the PC host didn't render (it renders only the
  captured frames); not checked with `SSA_VK_ALL=1`.
- Switches: `SSA_GX_CPUXF=1` the old CPU transform (word 0 bit 31, same shader); `SSA_GX_SLOWFETCH=1`
  the reference `fetch()` (also used while recording, since it reads RAM through `readRam`). The
  "written" log line and `gxReport` give the vertex-loop time.

Next for the renderer:
1. **Fetch on the GPU too** (if the Jaguar needs it: ~5 ms here may be ~20 ms there): give the GPU
   guest RAM (import it, or allocate guest RAM from GPU-visible direct memory on the PS4) and let
   tev.vert read the FIFO's vertex bytes and the arrays big-endian, so the CPU copies only the
   inline vertex bytes. Or spread `fetchDraw` over worker cores.
2. **Speed:** the FIFO arrives through one fault per gather-pipe write. Give the gather pipe a real
   memory page and pick up the bytes in batches (at frame end or on a timer), not per write.
3. **On the PS4:** the backend on the kit's RADV with the kit WSI, presenting the XFB scaled to
   1080p, plus an internal-resolution scale for the EFB (the AAA settings).

## The PS4 app (2026-11-15, v00.01: built, on .90, not run yet)

`ps4/` builds the same runtime files as the PC host (recomp/*.cpp, Crema's interpreter, recompiler
glue and translated-code loader from `~/ssa-build/crema`) with ps4-kit: `SSA_VERSION=00.01
ps4/build.sh` (WSL) → `out/Skylanders-SSA-vXX.YY.pkg`, title **SKYL00001**, build tree
`~/ssa-build/ps4`, symbols in `~/ssa-build/ps4/symbols`. What differs from the PC host:
- `ps4/ps4_main.cpp` replaces the test driver's main: log `/data/SSA/ssa.log` (previous run
  `ssa-prev.log`), settings `/data/SSA/env.txt` (one `VAR=value` per line, the PC's switches;
  `VAR=` removes a default), then guest memory, the recompiler's tables and `ssaRun`.
- `recomp/host_ctx.h` + `host.cpp`: the signal context (FreeBSD mcontext at 0x40 on the PS4) and
  guest memory. PS4: 4 GiB reserved at 0x6000000000 (sceKernelReserveVirtualRange), MEM1/MEM2 one
  run of direct memory each, mapped at all three views; 16 KiB pages (the MMIO single step maps a
  16 KiB page over the register page). The second tick is a thread (no SIGALRM).
- `gx_vk.cpp`: Vulkan functions from a table (`vk_fns.h`, through vkGetInstanceProcAddr: RADV is
  linked in); every frame rendered (`SSA_VK_ALL=1` by default) and presented: the XFB blitted into a
  1920x1080 swapchain image on the kit's VideoOut surface, 4:3 pillarboxed (`SSA_WIDE=1`: 16:9),
  FIFO; `SSA_VK_PRESENT=0` off.
- `ps4/ps4_input.cpp`: the first user's DS4 as the remote + Nunchuk (fixed mapping until the binding
  layer): Cross A, Circle B, Square 1, Triangle 2, Options +, touch pad -, R3 Home, D-pad, left stick
  = Nunchuk stick, L1 C, L2 Z. `SSA_PRESS` (scripted) replaces it when set.
- Not in the package (the game's files): `/data/SSA/disc/` (the extracted DATA: sys/, files/, ...),
  `/data/SSA/ssa.aot` (the translated code, from `~/ssa-build/p1`), `/data/SSA/nand/` (XX/nand),
  `/data/SSA/figures/` (Spyro created at the first start, `SSA_PORTAL`).
Still single-threaded (CPU, devices, GX decode and the Vulkan submit+wait on one thread); no audio.

Console runs (2026-11-16, .90):
- v00.02: the second mapping of a direct-memory block failed (EBUSY 0x80020010) -> host.cpp turns
  on `sceKernelEnableDmemAliasing` first.
- v00.03: crashed at the first exit from translated code (the bridge's `jmp [r15+table]`, slot 0):
  the jump table's block for address 0 was never committed -> `PPCRecompiler_allocateRange(0,
  0x1000)` in ps4_main.cpp, as the PC driver does.
- v00.04: boots like the PC (AOT 0.3 s, disc, apploader, Wii Remote over the emulated BT, RADV
  device, swapchain, 2 flips), then hangs at ~3 s in `__AI_SRC_INIT` (0x803E3C10): it times one
  AISCNT step at each sample rate with OSGetTime and retries until it measures ~28 or ~37 us. Every
  AI access is a host fault, ~38 us each on the PS4 (26k a second), so it never measures in range.
  Done natively (mmio.cpp `aiSrcInitNative`: AICR's PSTAT/AFR cleared, as at its end); its
  translation switched off after the apploader (the code is checked first), the dispatcher catches
  its entry. `SSA_AI_SRC_NATIVE=0` turns it off. v00.05 still looped: `PPCRecompiler_invalidateRange`
  does nothing in this runtime (PPCRecompiler_init never runs, so the recompiler isn't
  'initialized'); patch-crema-wii.py now adds `PPCRecompiler_disableAt` (its own idempotent step),
  used from v00.06.
- v00.06: runs (AX command lists, ~42 presents a second, about 3x slower than the PC host in
  translated runs) but the screen flashed: the game copies each frame to one of its two XFBs in MEM2
  (0x10299D80, 0x103285A0) and also twice a frame to 0x00662840, which VI never shows. Every copy
  was presented. v00.07 presents only copies into an address VI's top-field register (0xCC00201C)
  has held; `SSA_VK_PRESENT_ALL=1` presents all, `SSA_VK_XFBLOG=n` logs the first n copies.
- v00.07/v00.08 "nothing on screen": not a rendering fault. The game was still LOADING: ~1 disc read
  (32 KB) a second, because ~24,000 hardware accesses a second each trapped (~38 us on the PS4:
  the whole second). The user saw the first screen after minutes. v00.08's profile (kit sampler,
  `SSA_PROFILE=1`): 27% kernel waits (the trap path's page maps), 13% unordered_map lookups of the
  emulated registers, 7% clock reads, 11% device updates done after every run of the loop.
- v00.09 (performance round 1):
  - Hardware accesses without traps: `recomp/patch-aot-hw.py` (format version 6). The generated
    code's memory helpers check for 0xCC000000-0xCDFFFFFF and call the runtime's `mmioHwRead` /
    `mmioHwWrite` through two new imports. Only in the functions that need it: `select` keeps the
    check in functions with a `lis` of a hardware high half, plus the ones a run saw trapping
    (`SSA_MMIO_HIST=1` prints "mmio: trapped in <entry>"; list in analysis/hw-trapped.txt);
    everything else compiles as before (code size 36.6 MiB, same as v5; checking every access
    made it 96 MiB). Anything missed still works through the trap. `SSA_HW_DIRECT=0` turns it off.
    Rebuild: patch-aot-hw.py crema <src>; then aot <dir> + select <dir> <main.dol> <list>, aotbuild.
  - The run loop polls devices and interrupt lines every `SSA_POLL_TSC` ticks (32768) or at once
    after a hardware write (not GX FIFO) or when EE turns on; the interpreter runs batches of up
    to 32 instructions (`SSA_INTERP_BATCH`). PC: the loading stretch went from 4-8 XFB copies a
    second to ~100.
  - Emulated registers in flat arrays (mmio.cpp RegFile) instead of an unordered_map.
  - The status line's "time:" split (translated / renderer / GPU wait / interpreter, accesses and
    how many trapped, XFB copies); `SSA_TIMES=0` off. DS4 button changes logged (first 200).
- v00.09 on the PS4: loads at up to 120 XFB copies a second, reaches the logos, title and intro
  cutscene; the DS4 works (Cross = 0x4000 logged). Two problems left, both fixed in v00.10:
  - Movies (logos, intro cutscene) showed as 4 squeezed, comb-striped copies (user's clip). The
    movie player keeps its Y/U/V planes LINEAR (pitch 640: r30 in the decoder's copy loop at
    0x801E4A04) and the game samples them as I8 textures through INDIRECT TEXTURING (two tiny IA8
    lookup maps, 128x4 and 64x4, wrap repeat) that turns the tiled reads into linear ones. The
    renderer had no indirect textures. v00.10: tev.frag does them as Dolphin's shaders do (fixed-
    point coordinates in 1/128 texels, up to 4 indirect stages, BP 0x10-0x1F commands with format
    masks, bias, the 3 matrices of BP 0x06-0x0E or dynamic S/T, wraps, add-previous); GpuState grew
    to 688 bytes. The PC had shown grey there (the movies weren't decoded in time on lavapipe), so
    the PS4 clip was what showed it. PC check: logos, Eon's intro, menus, first level load all right.
  - ~50% of the time in the interpreter (9M instructions a second): code the entry list missed.
    The big decoder function at 0x801E42D0 was cut off by the analysis at its switch (bctr at
    0x801E4A00), and ~400 functions are only called through pointers. `SSA_FIND_ENTRIES=1` logs
    untranslated call targets, bctr targets and interpreter starts ("entry-found <addr> <count>");
    the ones seen >= 1000 times were added (~/ssa-build/p1/entries2.txt, copy in
    analysis/entries-runtime.txt: 24,678 entries) and the code translated again (8 s). PC: the
    interpreter's share 10% -> 1-2%. Regeneration script: entries file -> jittest JT_AOT_OUT ->
    patch-aot-hw.py select -> aotbuild.sh (scratchpad regen_aot.sh).
  - Diagnosis tools added: `SSA_GXREC_AT=frame`, `SSA_GXREC_MAXDRAWS=n` (record a chosen frame),
    `SSA_GX_INFO` now prints textures, TEV orders, texgens and all texcoord scales,
    `SSA_VK_TEXLOG=n` (new textures), `SSA_VK_DUMPTEX=address` (decoded PNG + raw bytes),
    `SSA_LCDMA_LOG` (locked-cache DMAs; patched into the Crema copy by scratchpad lcdma_log.py).
- v00.10 on the PS4 (2026-11-17): movies right, 0 trapped hardware accesses, interpreter ~4%.
  The main menu and everything after it flickered: every other frame lost its background. Cause:
  gx_vk beginRendering() zeroed the pending draw's scissor, and flushDraw begins rendering for that
  draw, so the first draw after each frame start or EFB copy was clipped away (the menu draws its
  sky first in every other frame). Fixed in v00.11 (PC: 300 menu frames in a row all right).
  Found with `SSA_FRAMES_FROM=frame` (new: captures start there) + `SSA_FRAMES_EVERY=1`, a GXREC
  of the menu and gx_replay with SSA_GX_ONLY/SKIP; SSA_GX_INFO now also prints PE modes, TEV
  combiners and alpha compare.
  - v00.12: the first level flickered too (scene every other frame). The game draws the 3D scene
    every other frame and in between redraws a saved copy of it full screen (GENMODE cull 2) under
    the HUD. Our front faces were counter-clockwise, so that quad was culled, and level geometry
    (islands, houses, balloons) was missing. Clockwise is GX's front face (as Dolphin); SSA_VK_FRONT=ccw
    restores the old way. PC captures now save only XFB copies VI shows (the 0x00662840 copies made
    the regression sheets show black movie frames). PC regression: logos, movies, title, intro and
    the first level all right.
  - Speed: the first level sends ~35,000 draws / 430,000 vertices a frame; the PS4 shows ~6 frames
    a second there, the renderer's CPU side (FIFO decode, vertex fetch 25%, texture hashes 7%)
    about half the game thread. v00.11: texture hash in four lanes. Next: decode + draw on a
    GX thread of its own (Dolphin's dual core), then cheaper vertex fetch.
  - v00.13 (2026-11-17): v00.12's clip: menus steady, but the first level's sky flashed black in
    jagged patches. The sky is a sphere drawn after the scene at the far limit (viewport far 2^24,
    z scale 1, ZMODE LEQUAL no update): GX clamps fragment depth to 0xFFFFFF, the cleared value, so
    it passes; ours landed on 1.0 (> clear) and Vulkan's z clip cut its triangles at random. Now:
    depth clamp on, GX's near/far clipping as clip distances in tev.vert (Dolphin's way), the
    viewport's max depth 0xFFFFFF/2^24; depth copies round (exact d back). Found with the new
    `SSA_GX_INFO` depth line (XF 1005, projection, clip z/w, window depth) and lvonly-style
    `SSA_GX_ONLY=0-N` bisection of the scene copy (`SSA_VK_DUMP_COPIES`).
  - Sound (v00.13): AXWii voices mixed (dsp.cpp, after Dolphin's AXWii/AXVoice: ADPCM/PCM16/PCM8,
    linear SRC, volume envelope, low-pass, mixer buses with ramps, AUX send/return, output), the
    audio DMA's buffers to ps4_audio.cpp (32 -> 48 kHz, sceAudioOut, thread priority 400). Note:
    CMD_OUTPUT lists the surround buffer's address first (the game adds it to L/R next frame).
    SSA_AUDIO=0: off; SSA_AUDIO_WAV=file (PC); SSA_DSP_VOICELOG=n. First run's sound is PCM16 streams
    (music/voice); ADPCM effects unverified.
  - GX thread (v00.13, SSA_GX_THREAD=1, the PS4 default): the CP's bytes go through a 16 MB SPSC ring
    to a thread that decodes, records and presents; the CP breakpoint and PE draw-done/tokens become
    visible to the game only once that thread decoded up to them. PC: the SIGALRM tick must be
    blocked on that thread (it deadlocked). PC threaded runs got ~30% further in the same time.
  - v00.13 on the PS4: level ~15 XFB copies/s (the game alternates a 3D frame and a HUD-only one: ~7.5
    3D frames/s, the Wii's is 30); BOTH threads saturated (game thread 0% idle in the level, GX thread
    ~100%); gameplay black under the HUD; sound starved ~3% (AX frames 2950/10 s instead of 3333).
  - v00.14:
    - Gameplay black: the depth-of-field pass clears only the EFB's alpha (EFB copy with clear, CMODE0
      color update off, alpha on); we cleared all of the color. Clears now follow Dolphin's ClearScreen
      (masked channels: a quad with that write mask, copy.frag mode 512).
    - Gameplay ground black: XF texture matrix 60 (GX_IDENTITY, XF 0xF0) held garbage. LOAD_XF_REG's
      word count is 4 bits (1-16; Dolphin & hardware); we took 16 bits, so a header with stray upper bits
      copied FIFO bytes over all of XF memory. Tools: SSA_GX_TRACEDRAW=n (fetched vertex words),
      SSA_GX_XFWATCH=addr (XF loads over it), SSA_GX_INFO now prints each texgen's matrix rows.
    - Speed: two frames in flight in gx_vk (rings halved per frame, per-frame pools/fences/garbage;
      the GX thread waits only for the frame before last); vertex fetch jobs for worker threads
      (SSA_GX_WORKERS, PS4 3: plan made on the GX thread, vertex bytes copied, workers claim jobs by
      epoch|index CAS, gxFetchFinish before every submit); fetch plans cached per VCD/VAT/arrays/
      MATINDEX; no clock reads per draw (rdtsc); GX FIFO writes straight into the gather pipe
      (mmioHwWrite fast path; logAccess's first-access set became a bitmap); the time base from the TSC
      (calibrated, SSA_CLOCK_TSC=0 off); threads pinned (ps4_threads.cpp: game core 0, GX 1, fetch 2-4,
      audio 5); "gx: GX thread" 10 s time split in the log.
    - Sound: the audio DMA chains from the last buffer's end (no drift: the game makes 32 kHz exactly);
      ps4_audio keeps ~60 ms queued with +-0.5% rate steering and refills after running dry.
    - PC checks: level replay bit-identical with 0 and 3 fetch workers and with the draw-state reuse
      (BP-write version counter: textureFor calls 19,517 -> 733 for 8 frames); a PC run into gameplay
      shows the windmill island, textured ground, Spyro, HUD and fire breath.
    - Translated code: v00.12's SSA_FIND_ENTRIES log had 1,576 more entry points (call targets through
      pointers, switch targets, interpreter starts; the hottest seen 19,461 times); the 1,536 seen >= 20
      times were added (~/ssa-build/p1/entries3.txt, 26,214 entries) and the code translated again
      (scratchpad regen_aot.sh: 11 s + build): ssa3.aot, uploaded as /data/SSA/ssa.aot (the old one
      kept as ssa-old.aot; SSA_AOT=/data/SSA/ssa-old.aot in env.txt to go back).
    - Open: PC runs (scripted presses) hit unknown GX opcodes around frame 21,500-22,000 (level load):
      display lists at 0x9220xxxx (only 0x9F draws, 3-byte vertices) decoded with the wrong vertex size
      (7 in one run, smaller in another) and a top-level "02 80" leftover before an XF load; the PS4 logs
      v00.10-v00.13 have none. Suspects: a display list read while the game rewrites it, or partial
      command bytes (s_pending) kept across a GP FIFO re-init (GXSetGPFifo: CP read pointer/base
      written). The unknown-opcode log now names the display list, offset, the command before and VCD/VAT.
  - v00.14 on the PS4 (2026-11-17): gameplay renders; ~22-23 XFB copies/s in gameplay (~11 3D fps,
    v00.13: ~7.5); the GX thread now only 25-35% busy (vertices 7-10%, draws 7-9%, EFB copies 3-6%);
    the game thread is the limit (translated 82%, interpreter 7% = ~1.1M instructions/s, runtime ~20%,
    0% idle); sound starvation gone. User: controls "not playable", dialogue out of sync with the voices
    (the game runs at ~1/3 speed while the voices play in real time: frame-counted animation/subtitles lag).
  - v00.15: Wii Remote input reports (200 Hz) went into an unbounded ACL queue; the game, slower than
    the Wii, takes fewer, so the backlog grew and the controls lagged seconds behind. A data report now
    replaces one still waiting (bt.cpp acl(); "bt: 10 s: N input reports, M merged" in the log).
    Profiling switches: SSA_RUNPROF=1 (translated-code entries by address), SSA_PROF_FROM=seconds.
    The DS4 is read by a thread of its own (ps4_input.cpp, core 5, priority 600, every 4 ms):
    scePadReadState (~60 us) on the game thread had cost ~1.6% of it.
    - Next for the game thread: the AOT's call sites reload every live guest register after each call
      (~50 loads in big functions). Crema's C backend has an EABI mode (PPCRecompiler_cSetAbiCalls:
      r14-r31/f14-f31/CR2-4 kept in locals across calls) not used for this AOT; CodeWarrior's
      _savegpr/_restgpr/__save_fpr/__restore_fpr helpers (0x8034F280-0x8034F3xx) rewrite those on
      purpose, so they need special handling first. An AOT rebuild is ~5 min (87 files, ~15 s each).
    - Supervisor instructions in place (AOT format 7, recomp/patch-aot-sys.py, translated code in
      /data/SSA/ssa7.aot): mfmsr/mtmsr, supervisor mfspr/mtspr, segment registers, cache (dcbt, dcbz_l,
      icbi...), TLB, FPSCR and sync ops no longer leave translated code. A leave unwound the whole host
      stack to the loop and came back in at every return address; OSDisableInterrupts/OSRestoreInterrupts
      (~100k calls/s) and LCEnable's dcbz_l loop (512 a call, ~155 calls/s) made ~1M loop entries a
      second, and the v00.14 PS4 profile had the loop itself (ssaRun) at 29% of the game thread. Now
      the IML gives such an instruction a LEAVE marked 7 (paramU16) plus a segment enterable after it;
      BackendC stores what changed, then does it inline - the cache/sync hints (dcbt, dcbst, dcbf,
      dcbi, icbi, sync, eieio, isync, dcbz_l: no-ops in the interpreter too), mfmsr, and mtmsr when
      only EE (not turned on), RI or ME change - or calls `aot_imports.sys(h, at)` (ssa_run.cpp aotSys:
      the interpreter runs that one instruction on hCPU) and reloads the registers the next segment
      reads; then it goes on. aotSys leaves only when the loop must act: an interrupt that MSR.EE just
      let in (devices polled first if due), an MSR change other than EE/RI/ME (FP off: the FPU
      switch), an exception. SSA_AOT_SYS=0: old behaviour. 1,410 sites (331 mfmsr, 628 mtmsr, 21
      hints inline, 430 calls); "time:" counts the aotSys calls as "in place"; SSA_RUNPROF=1 lists
      them by address. PC, gameplay from 330 s (aot3 format 6 / all-calls / inline): loop entries
      1.06M/s / 46k/s / 44k/s, interpreted 2.15M/s / 238k/s / 232k/s, aotSys calls - / 823k/s / 96k/s,
      XFB copies 61.6 / 73.9 / 75.4 a second (a WebKit build shared the PC during the last). What the
      runtime still runs, per second: mtmsr turning EE on in OSRestoreInterrupts (0x80383E78) ~43k,
      context code (OSLoadContext's SRR/SPRG moves, OSSetCurrentContext's FP-off mtmsr) ~25k, locked
      cache DMA SPRs ~15k; interpreted: the 0x500/0x900 vectors ~155k instructions, idle-loop exits
      ~58k; ~2.7k external + ~1.9k decrementer interrupts and ~13k FPU switches. The Crema copy:
      patch-crema-wii.py, patch-aot-hw.py crema <src>, then patch-aot-sys.py crema <src> (format 6
      originals in ~/ssa-build/crema-f6-backup); then a new translation (regen_aot.sh).
    - Hot spots in the v00.14 PS4 samples (whole run): ssaRun 29%, [translated code] 9%, guest
      0x8037A770 (b 0x8037AA70: a 0x524-byte FP function using CodeWarrior's save helpers) 6.7%,
      fifoWrite 3.8% + mmioHwWrite 3.4% (gather pipe), interpreter 3.5%, idle loop 0x803873A0 3%.
  - v00.15 on the PS4 (2026-11-18): reaches the village; no input backlog any more (bt: 0 reports
    merged), but "whatever I press it goes to the same place" (clips: Spyro always at the same edge
    of the island; the HOME Menu in the clips came from R3 presses while trying buttons). Cause: after
    detecting the Nunchuk (0x55 to extension register 0xF0) the game writes 0xAA to 0xF0 and a random
    16-byte key to 0x40-0x4F - the extension's data is then ENCRYPTED - and bt.cpp only logged "not
    emulated yet": the game decrypted the plain stick, C/Z and calibration bytes into garbage (a stick
    held in one direction). v00.16: Dolphin's WiimoteEmu/Encryption.cpp as recomp/wiimote_crypt.cpp
    (made by scratchpad mkcrypt.py, GPL like bt.cpp); register reads of 0xA4xxxx and the reports'
    extension bytes encrypted with KeyGen1stParty tables while 0xF0 = 0xAA (bt.cpp extEncrypt). PC:
    the key matched an index (no fallback). SSA_PRESS also takes SUP/SDOWN/SLEFT/SRIGHT (the stick
    pushed for 2 s) for PC tests.
  - v00.16 on the PS4 (2026-11-19): controls work (the user plays through the village to Old Town);
    gameplay ~28-30 XFB copies/s (v00.14: ~24); game thread 100% busy, interpreter ~1%, loop entries
    ~17k/s. Gameplay samples: GX FIFO writes (mmioHwWrite + fifoWrite + gxFifoData) ~13%, the AX
    mixer ~5% (most of it copying every voice's 160-word block in and out, running or not), guest
    0x8037AA70 ~3% (AXFX reverb on the CPU: delay lines in floats, lfsx/stfsx), the rest a long tail of
    translated game code. User: "the white is glowing badly" (clip: Rizzo's cap, the Skylander Info
    panels and tooltip text, the cloud sea blown out white).
  - v00.17:
    - THE GLOW: nearly every draw sets BP 0x42's destination alpha constant - the game's per-object
      glow / depth-of-field weight in the EFB's alpha (0 for most, 13/20/77 for some), read back by its
      post-process (an IA8 half-size copy of luminance + alpha, then blurred copies). ~1,300 blending
      draws a frame use it too; gx_vk applied the constant only to draws that don't blend, so
      translucent draws (clouds, UI, effects) wrote their own alpha into the mask and glowed white.
      Now dual-source blending as Dolphin: tev.frag (-DDUAL_SRC, tev_frag_dual.h) writes the constant
      as its first output's alpha and the TEV alpha as the second; blending pipelines with the
      constant (key bit 24) read SRC1_ALPHA for the colour factors and write alpha as ONE/ZERO.
      SSA_VK_DUALSRC=0: as before. The EFB copy log (SSA_VK_EFBLOG) shows the copy word and filter;
      copies now scale colour by the copy filter's weights / 64 (Dolphin with its filter off).
    - GX FIFO writes inline (AOT FORMAT 8, recomp/patch-aot-gp.py, /data/SSA/ssa8.aot): translated
      stores to 0xCC008000-1F append to mmio.cpp's gather buffer through new imports (gp, gpFill,
      gpBurst = mmioGatherBurst) instead of aot_hw_stN -> hwWrite -> mmioHwWrite -> fifoWrite; only
      with the GX thread and no access log/histogram (mmioGatherInline); SSA_AOT_GP=0 off. Patched into
      the existing format 7 C++ (aot mode: aot5 -> aot6, no new translation). "time:" shows KiB GX FIFO.
    - AX: voices that aren't running are skipped (only their link read) in axProcess.
  - v00.18 (2026-11-19, the quick menu; on .90, untested): hold L1 L2 L3 + R1 R2 R3 (all six) to open
    it; the game pauses (pollDevices -> pauseGame, the time base stopped: s_tbPaused) and the GX thread
    presents the last frame shown (s_lastFrame) with the menu over it (gx_vk presentIdle, own command
    buffer, FIFO-paced). DolphinPS4's look (overlay.cpp: ImGui 1.89.5 draw lists, Nunito from
    /app0/fonts, PS4Glyphs icons). Pages: Main (Resume, Frame Counter Off/FPS/Detailed, Quit), Graphics
    (Aspect Auto/4:3/16:9, Game Widescreen = SYSCONF IPL.AR at the next start, Internal Resolution
    1x-3x at the next start: gx_vk s_scale scales the EFB, XFB, copies, scissors and clears; Texture
    Filtering Game/Smooth), Performance (Sound, vertex fetch threads, renderer thread - next start),
    Controls (DolphinPS4's Wii Remote + Nunchuk layout, press-to-bind with icons, Square clears, reset),
    Wii Remote (Pointer stick/gyro/touchpad/off, speed, Motion Controls from the DS4's sensors, PS Move,
    Rumble). Settings in /data/SSA/settings.ini (settings.cpp; env.txt still wins). Input:
    ps4_input.cpp reads the DS4 (motion on via scePadSetMotionSensorState; OpenOrbis's vel/acell are
    acceleration/rotation) and, only once the setting is on, a PS Move (sceMove*, -lSceMove) every
    4 ms; overlayInput first, then wiimapUpdate -> a WiiState the game thread takes (bt.cpp
    g_btReadInput); bt.cpp now sends IR camera dots (basic/extended, from the pointer; the game sets
    the camera up) and passes the rumble bit (g_btRumble). The NAND's SYSCONF already says 16:9
    (IPL.AR=1): the game draws anamorphic 16:9 (round HUD portrait at 16:9), so v00.17's 4:3 pillarbox
    squeezed it; Aspect Auto now shows 16:9. PC: gx_replay SSA_REPLAY_LOOPS=n, SSA_MENU_SCRIPT
    ("frame:press,...,frame:shot"), SSA_MENU_SHOTS=dir (1080p PNGs of the presented picture).
  - v00.19 (2026-11-19, the game thread; on .90 with /data/SSA/ssa9.aot, untested): the v00.17
    samples had the AOT's code at 91% of the game thread, the runtime 9%, and no hot spot but one:
    - The AXFX reverb (the SDK's HandleReverb, 0x8037AA70; translated as part of its 4-byte branch
      stub 0x8037A770, so the profile named that one) at 8.1%: ~44,400 guest instructions per 96-sample
      frame, ~333 a second. dsp.cpp axfxReverbHiNative does the same single-precision operations in the
      same order on the game's state; ssa_run.cpp switches both translations off (PPCRecompiler_
      disableAt) and runs it from the loop. SSA_REVERB_CHECK=n (SSA_REVERB_CHECK_FROM=s): the call also
      runs as the game's code in the interpreter from the same state and all 28 regions it touches are
      compared - identical on the PC. SSA_REVERB_NATIVE=0: translated.
    - Calls in translated code (recomp/patch-aot-calls.py on BackendC, translation with
      JT_AOT_KEEP=8034f280-8034f3b0, /data/SSA/ssa9.aot; same format 8, ssa8.aot still loads): EABI's
      kept registers (r1 r2 r13 r14-r31, f14-f31's first halves, CR2-4) aren't loaded again after a
      call (except after CodeWarrior's save/restore helpers in that range), and an entry at a
      function's start loads only what its code reads (liveness across calls and format 7's in-place
      sys instructions). In a hot function: 10 -> 4 loads after a call. Same results as aot6 on the
      PC over 5 runs; the scripted PC run has 10-15 s stretches without frames around 245-320 s with
      the old code too (not the translation).
    - GX FIFO bursts batched (mmio.cpp): with the GX thread they collect up to 2 KiB and go to the FIFO
      and the GX thread together, before any other hardware access and at every device poll;
      cpUpdate hands contiguous runs to gxFifoData. SSA_FIFO_BATCH=0: each burst at once.
    - The PC can't show the gain (its XFB rate is the VI's); the PS4's samples (env.txt profile) will.
  - v00.20 (2026-11-20, the translated code; on .90 with /data/SSA/ssa15.aot, untested): same format 8
    (ps4_main.cpp falls back to ssa9.aot, then ssa8.aot; copies in out/ssa-v00.NN.aot). Measured on
    the PC as busy samples per XFB copy: perf on the game thread over 60-330 s of the scripted run
    (prun-style; perf has to start the process), the OS idle loop aot_80387480 left out, builds run
    in pairs started together (the level drifts between pairs by a few %). v00.19's ssa9.aot 4.0;
    ssa15.aot 3.47 against 3.63 for ssa12 in the same pair (ssa12 alone 3.23) - about 23% less
    game-thread work per frame. Translated again with regenall.sh (BackendC from the format 8 backup,
    patch-aot-calls.py + -float.py + -mem.py, JT_AOT_KEEP=8034f280-8034f3b0), then the mfcr pass:
    - Single floats (recomp/patch-aot-float.py on BackendC): single-precision results were rounded
      through aot_f2d, an asm statement clang can't see through, so every double<->float conversion
      stayed. Now plain (double)(float) casts, single loads as (double)aot_u2f(bswap), and
      aot_single_bits takes a double that is exactly a float straight to its bits: LLVM narrows
      fptrunc(op(fpext a, fpext b)) to the float op. FP conversions 7% -> 4.5% of the executed
      instructions; -12..15% busy (ssa10/ssa11).
    - Guest memory (patch-aot-mem.py): aot_ld*/st* through memcpy (not volatile), m no longer
      __restrict, stwcx a plain compare and store; dead loads now go. aotbuild.sh -fno-builtin-mem*
      (the game's store loops became memset calls the file can't import). -5% (ssa12).
    - mfcr (patch-aot-mfcr.py, on the C++: `patch-aot-mfcr.py aot <dir>`): the IML's 64 dependent
      steps over 32 CR bytes. In a function's start segment before any CR write: two SSE2 loads of
      hCPU's CR bytes, pmovmskb, bitreverse (55 places); elsewhere an OR tree (280). -4.5% (ssa15).
    - Tried and dropped: always-inline copies of leaf functions up to 64 guest bytes
      (patch-aot-inline.py: 5832 functions, 22818 calls; +9.5% busy - register pressure, code size)
      and the busiest functions first (aotbuild.sh AOT_ORDER=file, lld symbol ordering; +5%).
    - JT_AOT_REAL (every function as JIT code and as translated code from random states, compared):
      180 differ against 85 for ssa9; the new ones are garbage-pointer loads that no longer fault (dead
      loads removed) and data at 0x804C..0x805x taken as functions. 330 s scripted PC runs: no fault.
    - Left on the PC's game thread: guest memory accesses 35.6% of the instructions, ALU 22.9%, hCPU
      loads/stores 16.1%, spills 9.1%, branches 9%; the runtime ~13% of the busy time (ssaRun 3%,
      ssaTimeBaseRaw 1.8%, the interpreter 1.2%, the AX mixer ~1.1%, native reverb ~1%). 30 fps
      still needs ~2x. Next ideas: no stores of dead volatile registers at calls and returns (EABI;
      ~3%, the differential test would have to ignore them), cheaper time base reads.
    - PC tools: ssa_run.cpp logs "ssa: aot_<entry> at <host address>" (perf symbols of the loaded
      file); driver.cpp JT_REPORT=n (JT_AOT_REAL's report limit), JT_AOT_KEEP_PARTS; mmio.cpp
      SSA_VI_HZ=n (VI interrupts a second; it didn't make the PC CPU-bound).
  - v00.21 (2026-11-20, on .90, untested): the user turned PS Move on in the quick menu and the app
    ended at once, then at every start (settings.ini psmove=on; the log stopped at "input: PS Move:
    init", no crash log). libSceMove is not among the system modules loaded at start (ps4-kit loads
    SystemService, UserService, VideoOut, AudioOut, Pad), and a call into a library whose module isn't
    loaded ends the app without a word. ps4_input.cpp now loads it first (sceSysmoduleLoadModule
    ORBIS_SYSMODULE_MOVE = 0x8F, a public module id: there is no internal one), says so if it doesn't
    load or start, tries sceMoveOpen again every 3 s while there's no Move, and keeps a mark file
    (/data/SSA/psmove-starting) from the module load to the first read: found at the next start, the
    setting goes off with a message instead of ending the app again. overlayToast(text, ms): a toast
    from before the first frame is shown from the first frame on.
  - v00.21 on the PS4 (2026-11-20): PS Move fixed (module 0, init 0, a handle; no Move connected).
    Village: 32-46 XFB copies/s = 16-23 fps presented (the user: "sometimes 23"); in play the game
    thread is ~100% busy (idle 0-3%), 94-96% of it translated code, the profile flat (the busiest
    guest function ~0.7%); the GX thread ~50%, GPU wait 0%. So the frame rate is the game thread's
    speed: 30 fps everywhere needs ~1.3x (the 23 fps places) to ~1.9x (the 16 fps ones). Also seen:
    the game opens/closes /dev/net/ip/top, kd/request and ncd/manage in a loop (8 requests a loop,
    20-56 loops/s): ios.cpp logged each (~280 writes/s); now the first 20 and every 1000th.
  - v00.22 (2026-11-20, the translated code again; on .90 with /data/SSA/ssa16.aot, untested):
    - The register save/restore helpers expanded in place (recomp/patch-aot-expand.py on Crema's
      PPCRecompilerImlGen.cpp; translation with JT_AOT_EXPAND=8034f280-8034f3b0): a bl into the range
      whose code up to its blr is only lwz/stw/lfd/stfd off r11 is decoded in the caller (the same
      generators the helper itself is translated with) and LR set as the bl would; it no longer ends
      a basic block. 3820 call sites -> 0 (the 229 left call 8034f224 and 8034f3b0, outside the range).
      _savegpr_27/_restgpr_27 alone ran 88 million times in a 400 s PC run. JT_AOT_REAL with
      JT_AOT_EXPAND for the JIT side too: 181 differ (ssa15: 180), the known artifacts; without it the
      JIT stops at the bl (the test runs a function until it leaves its code) and ~2200 "differ".
    - Profile-guided build (recomp/pgo.sh): the C++ built with LLVM's IR counters
      (-fprofile-generate -mllvm -disable-vp=true: nothing imported, so no profile runtime), the
      scripted PC run with it, ssa_run.cpp SSA_AOT_DUMP=file,from,size writes the counters section
      (from the entry function's address) at the end, pgo-profraw.py makes a raw profile (format 10:
      header + the file's __llvm_prf_data + the counters + __llvm_prf_names), llvm-profdata; the OS
      idle loop aot_80387480 counted ~3e14 (its trip count added at once) and would make everything
      else look cold: pgo-zero.py zeroes it. Then -fprofile-use with -mllvm -pgso=false (code the run
      never reached - 25 of 41 MiB - is compiled for speed as before, not for size). The objects split
      into .text.hot (4.2 MiB), .text (11.8) and .text.unlikely (24.9).
      On ssa15's code, side by side: 4.09 against 4.47 busy samples per XFB copy, -8.6%.
    - Four builds side by side (scratchpad multi.sh; 330 s, none faulted): ssa15 2.125, ssa16p
      (expanded + profile-guided: shipped as /data/SSA/ssa16.aot, out/ssa-v00.22.aot) 1.808 = -14.9%,
      ssa16pk (the same objects linked with -z keep-text-section-prefix: hot 4.3 MiB together) 1.853 =
      -12.8% (layout again worse on the PC, as v00.20's hot-first order; might differ on the Jaguar,
      untested), ssa16pl (ThinLTO + the profile) not measurable: its idle loop's trip count is
      computed at once as in the instrumented build, the dispatcher spins instead (ssaRun 52%), and
      without that it looks no better - dropped. (Levels drift between sessions: ssa15 was 4.47 in
      the first pair, run while builds were compiling; compare only within one run.)
    - Expected on the PS4: ~1.17x the frame rate of v00.20/21 (16-23 -> ~19-27 fps); 30 everywhere
      still needs more. Next: a calling convention that passes guest registers in host registers
      instead of hCPU, the code in 2 MiB pages on the PS4 (TLB), the hot-grouped layout tried there.
    - Tooling lesson: `pgrep -f pattern` in a wait loop matches the loop's own command line.
  - v00.23 (2026-11-20, the one-file package for the PS4 Pro .43; untested): the user: "make sure this
    runs on pro through boost mode, and create one big package includes everything and send it to .43".
    - Pro: still a PS4 app (no NEO flag: the kit's RADV knows only Liverpool, make-pkg.sh's note), so
      a Pro runs it at 2.13 GHz only with Boost Mode on (Settings > System; MGR measured 2.09 GHz on
      .43). ps4_main.cpp logs "ssa: CPU <MHz>: a PS4 / a PS4 Pro, Boost Mode on|OFF" (ps4_kit_cpu_mhz,
      sceKernelHasNeoMode/IsNeoMode; boost = Pro, not its own mode, >= 1900 MHz), the detailed frame
      counter shows "CPU <MHz> (Boost)" (overlaySetCpu), and a Pro without Boost Mode gets a 10 s
      toast to turn it on. The time base measures the TSC against the clock (no 1.6 GHz constant).
    - One-file package: ps4/build.sh SSA_PKG_FULL=dir hard-links dir's disc/ (the extracted disc,
      307 files, 4.0 GiB), ssa16.aot and seed/ into the stage -> out/Skylanders-SSA-vXX.YY-full.pkg
      (the kit's fixed PkgTool past 4 GB). ps4_main.cpp: the disc and the translated code from /data/SSA
      if there (.90's way), else /app0; seedFromPackage copies seed/nand (to nand.new, then renamed),
      figures/spyro.sky and settings.ini to /data/SSA at the first start when /data/SSA has none.
      dir = ~/ssa-build/pkg-full: the disc from XX/disc/DATA, ssa16.aot = ssa16p.aot, seed/ = .90's
      /data/SSA nand (SYSCONF, setting.txt, the four save slots), figure and settings as of today.
      The game's own data: for the user's consoles only, never published.
- The next wall: hardware accesses cost ~38 us on the PS4 against a few on the PC. Every GX
  gather-pipe write (0xCC008000) is one, so drawing will be slow until FIFO writes stop faulting
  (backpatch the faulting host instruction into a call, or translate stores to 0xCC008000 as calls).

## Requirements from the user (2026-11-14)

- **Quick menu with AAA-style settings, and rebindable controls.** An in-game overlay menu (opened by
  a reserved DS4 combination) with graphics/audio/control pages in the style of a modern AAA settings
  screen, and a binding page. All input goes through one remappable layer: DS4 → emulated Wii Remote
  buttons/pointer/motion, Nunchuk stick/C/Z, and Portal actions. Bindings are saved per profile.
  Built with the PS4 runtime and the renderer (P3), since it draws over the game.
- **USB on the PS4 is uncertain.** The Wii Remote doesn't need it: the dongle and the remote are
  emulated in software and fed from the DS4 through `scePad`. For the **Portal of Power**:
  - An **emulated portal is the primary path**: figure dumps (`.sky`), placed and removed from the
    quick menu, behind `/dev/usb/hid`.
  - A real USB portal through `sceUsbd` is an optional extra, only if it proves to work on the
    console.

## Route: static recompilation (not a matching decomp)

A matching decomp of 1.27 M instructions of C++ (Alchemy + Havok + game) is a multi-person, multi-year
job. A static recomp gets native speed for the whole binary; individual functions can be replaced by
hand-written C/C++ later where needed (renderer, timing, hot spots).

### P0 — Analysis (days)
- Split and analyze with **decomp-toolkit (dtk)**: function boundaries, jump tables, vtables, data vs. code.
- Match RVL SDK, MSL, Havok and Bink functions by signature, so their call sites can be HLE'd by name.
- Ghidra project for manual work (REA/Ghidra 12 at E:\tools).

### P1 — Recompiler (2–4 weeks)
- Start from the PowerPC lifter in the BOTW/SMW recompiler (iHaiDeeZ/BOTW branch `recomp`).
- Add: Gekko paired singles (`ps_*`), `psq_l/st` with GQR dequantization, CodeWarrior quirks, `mftb`/decrementer.
- Output C for every function, plus an indirect-call table for vtables/function pointers.
- Guest memory: MEM1 (24 MiB) + MEM2 (64 MiB) at fixed host addresses, big-endian loads/stores.

### P2 — OS / SDK HLE on ps4-kit (2–4 weeks)
- OS: threads (cooperative, priority-based; keep the game's priorities and core layout, see the ReXGlue lesson), mutexes, alarms, arena/heaps.
- DVD → files from `/app0` (disc tree as-is). NAND saves → `/data`.
- AX → mixer → AudioOut. Bink → recompiled Bink decoder (it's in the DOL) or native Bink.
- WPAD/KPAD → **DS4** (Wii Remote + Nunchuk layout; pointer → touchpad/gyro where the game uses it).
- **HID → Portal of Power**: (a) real USB portal via sceUsbd (the Wii portal is the same USB HID device family as the PS3/PC one; to be confirmed for a PS4-era portal), (b) emulated portal reading figure dumps (`.sky`), like Dolphin/RPCS3.
- Network libs stubbed (offline).

### P3 — GX → Vulkan renderer on kit RADV (the big one: 1–3 months)
- ~~HLE at the GX API level~~ → done at the **FIFO level** instead (R0), as Dolphin does: one decoder for everything, display lists included.
- TEV → one uber-shader (R2), so there are no shader compiles at run time; specialized shaders later only if the uber-shader is too slow on the PS4's GPU.
- EFB copies (shadow map, bloom, DOF at 1/8 screen — see `_bloomScreenAtEighthSize`, `_dofScreenAtEighthSize`) → render-to-texture, kept on the GPU.
- Wii texture formats (CMPR/I4/I8/RGB5A3/...) decoded once at load time.
- Output 720p/1080p, 16:9 (game already has widescreen).

### P4 — 60 fps (1–4 weeks)
- First measure what the Wii build runs at and how Alchemy's `igFrameTime` steps the game. If logic is tied to a fixed 30 Hz tick,
  60 fps needs either (a) a 60 Hz tick (check Havok step, animation, scripts, UI timers) or (b) rendering at 60 with interpolation.
- CPU budget: recompiled code for a 729 MHz CPU should fit easily on one Jaguar core; the renderer's draw-call cost is the main risk.

## Rough total
3–6 months of focused work to a playable 60 fps build; the renderer (P3) and the 60 fps timestep (P4) carry most of the risk.
