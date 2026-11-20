# jittest: the recompiler against the interpreter, on the PC

Builds Crema's PowerPC recompiler (IML, register allocator, x86 backend) and Cemu's interpreter
from the working tree (`~/cemups4-build/src/Cemu`) for the WSL host, with stand-ins for the rest
of the emulator (`stubs.cpp`). `driver.cpp` generates random programs (float, paired-single,
loads and stores, integer carry arithmetic, compares and forward branches), runs each through the
interpreter and through the recompiled code from the same registers and memory, and prints one line
per program: `test interpreter-hash recompiler-hash same|DIFF|compile-failed`. The recompiled code
runs as on the PS4: AVX and MOVBE, no BMI2, MXCSR flush-to-zero and denormals-are-zero.

    bash build.sh objects && bash build.sh link     # ~/jittest-build/jittest
    ~/jittest-build/jittest seed count [length] [verbose]
    bash build-old.sh                                # ~/jittest-old/jittest: the recompiler as committed
    bash compare.sh seed count length                # old vs new on the same programs

Options (environment): `JT_OPS=a,b,..` only those instruction kinds (the `emitOne` cases),
`JT_SINGLES=1` registers start with single-precision values, `JT_NOBRANCH=1` straight-line,
`JT_REGS=n` float registers used (8: aliasing, 32: register pressure), `JT_DUMP=test` full result
of one program, `JT_BENCH=n` run each compiled program n more times and time it, `JT_SO=1` random
summary-overflow bits (the recompiler leaves CR0.SO alone, as Cemu).

Several entry points: `JT_LOOPS=1` adds loops (li/mtctr ... bdnz, or li r16 ... addic./bne, 1-3
rounds), `JT_ENTRIES=n` compiles each program with n more entries (loop heads, places inside loops,
branch targets, any instruction) and runs it from each of them as well, from the same state, against
the interpreter started there; the summary counts the differences per kind. Debugging: `JT_ONLY=test`
compiles and runs only that program (the others are still generated, so the numbering holds),
`JT_TRACE=1` prints each run before it starts and the x86 code's entry offsets (a crash in the
recompiled code then names its program and entry; the crash report gives rip as a code offset),
`JT_CODE=file` writes that x86 code (`objdump -D -b binary -mi386:x86-64 file`), `JT_IML=1` prints
the intermediate code after register allocation (a JITTEST-only hook in PPCRecompiler.cpp).
`JT_RPX=code.bin JT_FUNCS=list` compiles a game's real functions (`rpx2bin.py` output, the console's
cache/jit list); with `JT_MAPPED=1` only the areas Cemu maps are readable and the rest faults, as on
the console, and `JT_MAPPED=2` faults the same while calling everything readable (the boundary tracker
before 2026-10-22, which followed branches into unmapped memory: Breath of the Wild crashed at 0x7270).
`JT_CACHE=file`: the compiled code cache (PPCRecompiler.cpp, cache/jit/<title>.code on the console).
With random programs each one is recorded, written, its jump table entries forgotten and loaded
again, and the loaded copy (its x86 code at another address) runs instead of the compile, whose
code becomes int3; each program is also loaded once with one instruction changed (must be left
out). The result lines must equal a run without it. With `JT_RPX` every compiled function is
recorded, then the file is loaded (all made active) and loaded again over them (all "compiled
meanwhile"); `JT_CACHE_LOAD=1` only loads an existing file. Run under `setarch -R` so a second
process finds Crema's code at the same addresses (else the cache is "made by another build").
`bash cache.sh [code.bin list]` runs these (and, given a game's code, damaged files and changed code).
2026-10-22: 1,000 random programs (seeds 1-3, loops and 6 extra entries) identical through it;
BotW's 64,667 functions: 119 MB file, loaded in 0.12 s on the PC; a damaged end, records twice and
changed code handled.
Cycle Counter (BackendX64.cpp, `g_crema_cycles_in_register`): the remaining cycles live in R12 while
recompiled code runs; `JT_CYCLES_MEMORY=1` compiles with them in memory as Cemu does, `JT_CYCLES=1`
prints the cycles left after each recompiled run (stderr `cycles` lines). `bash cycles.sh [code.bin
list]` runs the same programs both ways: the result lines and the cycles left must be the same, and
a game's functions compile both ways (2026-10-22: 6,453 runs identical; Breath of the Wild's x86
code 101.7 MiB instead of 106.4).
Functions cut short (2026-10-23): `JT_CUT=1` leaves nothing mapped after a word picked per program while
it compiles, so the boundary tracker ends the function there; the blocks whose flow goes on past that
end now leave through the jump table (PPCRecompiler_GenerateIML; before, the whole function failed and
ran in the interpreter: 22 of Breath of the Wild's) and the run finishes in the interpreter. Exact
kinds with loops and 6 extra entries: 3,000 programs and ~3,700 entry runs identical (both Cycle
Counters); compiles that still fail there have a conditional branch past the cut (unsupported, and on
the console branch targets are always mapped, so the tracker takes them in). Kinds 67/68 (`JT_OPS`
only): fcmpo and ps_cmpo0, recompiled as fcmpu/ps_cmpu0 (they differ only in FPSCR, which recompiled
code doesn't keep); with moves alone they match exactly, mixed with cr0 record forms they differ in
CR0.SO like 19/37. In RPX mode the import stubs are zeros, so those functions still fail there.
For interpreter comparisons use the kinds that match exactly (no cr0 compares, which differ in CR0.SO):
`JT_OPS=0,1,3,8,9,10,11,12,13,14,15,17,18,20,21,23,28,29,34,35,36,39,40,41,46,47,48,49,50,51,52,54,55,56,57,58,59,60,61,62,63,64,65,66`.
2026-10-21: with up to 10 extra entries, ~45,000 entry runs matched the interpreter (once an fmadd's
NaN sign differed: register allocation changes with the entries).

Translated game code (2026-10-24; `Cafe/HW/Espresso/Recompiler/BackendC`, loader `CremaAot*` in
PPCRecompiler.cpp): the intermediate code written as C++ and compiled ahead of time by clang -O2.
`JT_AOT=1` (random programs): each also as C++, built into one library and run, against the JIT's
run (must be identical) and the interpreter's; `JT_AOT_LOADER=1` also builds them as a translated
code file (`aotbuild.sh`, as a game's) and loads it with Crema's loader per program (only the
program at kCode matches its hash; one instruction changed: nothing made active), running through
the jump table (stub, bridge, function); results and cycles must equal the JIT's, with either
Cycle Counter. A game: `JT_RPX=code.bin JT_FUNCS=list JT_MAPPED=1 JT_AOT_OUT=dir` writes every
function the JIT compiles as C++ (dir/aot_NNNN.cpp, 300 each; unchanged files are not written
again, so `aotbuild.sh dir out.aot` compiles only what changed) and aot_manifest.cpp, then
`bash aotbuild.sh dir out.aot` builds the file (cache/aot/<title ID>.aot on the console).
`JT_AOT_REAL=file.aot` (with JT_RPX): every function runs from random states as the JIT's x86 code
and as the file's function, until it leaves its code: CPU state, cycles and memory must be the
same (guest memory: the game's code and zeros read-only, a 2 MiB scratch area at 0x10000000 whose
words mostly point into it; runs that fault on both sides are left out; the JIT's jump through the
table to a garbage address counts as the same exit). `JT_AOT_CHAIN=file.aot`: the file loaded and
made active, real functions run through the jump table with the calls between translated
functions made directly and again with every transition through the bridge (must be identical);
with `JT_CACHE=file` also with the JIT's code of every function (compared and timed).
The generated C++ comes from the game's own files: keep it outside the repository, never share it.

CR bits dead at returns (2026-10-25, `PPCRecompiler_RemoveDeadCRWrites`, Compare Results Fast):
- In the game's own code (not graphic pack code caves or patched functions), a return leaves cr0,
  cr1 and cr5-cr7 dead.
- The eflags jumps (`IMLOptimizerX86_SubstituteCJumpForEflagsJump`) no longer store CR bits that
  Crema's CR liveness finds dead (`IMLSegment::cremaCrLiveOut`).
- `JT_EABI_RETURNS=0` is Compare Results "Fast, Keep at Returns". With the rule on, the comparison
  leaves cr0, cr1 and cr5-cr7 out, since every run ends with a return.

RPX mode prints the total x86 code size. `JT_CODE=file` with a one-function list writes that
function's code. `JT_PATTERNS=1` counts short jumps padded with a 4-byte NOP and compares with a
byte-sized imm32.

Breath of the Wild, 68,760 functions:
- 116.77 MB as before.
- 114.35 MB with the eflags change alone.
- 109.47 MB with both (-6.25%).
- 108.33 MB with compares against values that fit a byte using the imm8 form too (BackendX64.cpp, -7.2% in all).
- Before the imm8 compares: 218,746 padded jumps and 436,738 wide compares.

stfs of lfs values (2026-10-25, IMLOptimizer.cpp `g_crema_single_loads_known`, with Float Inputs: Fast):
- An lfs (cvtss2sd under denormals-are-zero) yields only values the rounding store (cvtsd2ss) gives
  back bit for bit. `scratchpad stfs_check.c` checked all 2^32 singles.
- Every MXCSR mode gives 0 differences, except flush-to-zero without denormals-are-zero (Float Inputs:
  Exact): 16,777,214 differ, every denormal. So the rule is off there.
- Exact stores in BotW: 160,704 -> 35,071 (ConvertToSingleNoFTZ, ~50 bytes inline plus cold code).
- BotW x86 code: 108.33 -> 99.30 MB (-15.0% against 116.77).
- `JT_SINGLE_LOADS=0` turns the rule off. Random programs of all kinds (seeds 21-24, 2,000 programs):
  result lines identical with the rule off and on.
- `JT_BYTES=1` prints the x86 code by intermediate instruction and where the remaining exact stores'
  values come from.

hCPU in RBP (2026-10-25, BackendX64.h `REG_RESV_HCPU`, `kCremaHcpuBias`, `HCPU_OFS`):
- Recompiled code keeps the guest CPU state in RBP, 128 bytes past its start, instead of RSP.
  Accesses lose the SIB byte, and one-byte displacements reach the struct's first 256 bytes.
- PPCState.h now puts gpr, cr, xer, cycles, spr, reservedMem and the temporary GPRs there, before fpr.
- RSP is the host stack inside recompiled code (== hCPU->rspTemp, 8 off 16-byte alignment):
  - HLE calls no longer switch stacks;
  - CALL_IMM reserves 0x28;
  - the translated code's bridge no longer saves RSP in RBX.
- The x64Gen emitters that took only RSP (mov/sub/bt/dec dword, the SSE moves) now take any base,
  with the shortest displacement.
- RBP left the register pool. That costs nothing: BotW's code was 99.13 MB without it, against 99.30 MB
  with it.
- With 10 registers, the allocator looped forever on BotW's 03f08570. It now explodes an active
  range that goes on into other segments when none frees a register for a local one. A cap of 4,000
  rounds remains a safety net: BotW needs at most 381; on the cap the function is interpreted.
- Built from a copy of the tree first (`JT_SRC`/`JT_OUT` for build.sh, `JT_BIN` for cache.sh).
- Result lines identical to the RSP build:
  - seeds 41-44, all kinds;
  - 45, exact kinds: 1,000 the same;
  - 46, cut;
  - 47-48, cycles both ways, with the cycles identical;
  - 49, Keep at Returns.
- cache.sh is the same. JT_AOT and JT_AOT_LOADER: 0 differ.
- BotW x86 code: 99.30 -> 90.65 MB (RBP) -> 86.84 MB (layout), -25.6% against the 116.77 MB of
  Lab v03.32 before 2026-10-25.

Guest Returns (2026-10-25, BackendX64.cpp `g_crema_host_returns`, Settings > Diagnostics):
- Predicted: bl/bctrl CALL their target through the jump table, and blr RETs with the target in EDX, so
  the CPU's return stack predicts it.
- The return site checks EDX against its own address. A return elsewhere goes through the jump table,
  via one stub per function.
- The entry pushes a base frame whose stub does the same and pushes itself again.
- HLE and CALL_IMM align RSP dynamically (the old RSP in R14). HLE also cuts a stack grown more than
  1 MiB back to the base frame.
- bctrl carries its return address in op_macro.param.
- `JT_HOST_RETURNS=0` gives Jump Table.

`JT_JIT_CHAIN=n` (with JT_RPX and JT_CACHE):
- n real functions run in the JIT's code with their calls and returns, from random states.
- One line per run goes in JT_JIT_CHAIN_OUT, so two builds can be compared run by run (same seed, same
  runs; timed-out runs left out).
- Two identical builds: 0 of 2,995 differ, the ticks 4% apart (a loaded PC).
- Predicted against Jump Table: 0 of 2,995 differ.
- BotW x86 code 86.87 MB (Jump Table) -> 91.17 MB (Predicted, +5%: the site checks).
- The PC can't tell whether it pays (its indirect jump prediction is good); the PS4 has to.

Tests, both modes:
- 1,000 random exact-kind programs (loops, 6 extra entries): identical.
- cycles.sh: identical.
- cache.sh: identical.
- JT_CUT (1,000 programs): 1 DIFF, a NaN sign in stfd, the same in both modes.

Call-site scans (session scripts: does code after a call read a volatile register before writing it?):
- CR fields: never in BotW or Minecraft, except FPS++ code-cave callees and `crset 2; bl 0x02000000
  (nop; blr); beq`.
- GPRs: yes. GHS keeps values in r0/r5-r12 across calls it knows (`oris r9; bl; rlwinm r5,r9`).
  Never treat volatile GPRs as dead at calls or returns.

Known interpreter/recompiler differences that are Cemu's own (old and new recompiler alike):
single-precision multiply-adds (the interpreter rounds frC to 25 bits), fsel, ps_madd/ps_nmadd,
lfs/psq_l/psq_st of special values, CR0.SO. NaN sign/payload can differ when both operands of an
operation are NaNs (`nancheck.py old new` on two `JT_DUMP` outputs tells).
