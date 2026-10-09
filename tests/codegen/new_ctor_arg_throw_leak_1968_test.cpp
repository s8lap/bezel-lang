// Codegen — `new T(...)` reclaims the freshly-allocated instance when a LATER
// constructor ARGUMENT throws during evaluation (issue #1968, §28.3).
//
// ROOT CAUSE: lowerNew (src/codegen/LlvmBackend_Exprs2.cpp) calloc's the
// instance BEFORE it evaluates the constructor arguments (buildOrderedArgs).
// When a later argument's evaluation throws — e.g. `new C(1, MakeIntOrThrow())`
// — control leaves the argument list WITHOUT running the constructor, so `obj`
// is allocated but never constructed (every field is calloc-zeroed) and bound
// to no `using var`/scope: the ordinary scope-end / owned-local unwind disposal
// never reclaims it → the raw block leaks (LSan: 24 bytes for the plain repro,
// 32 for the owned-arg repro).
//
// FIX: lowerNew installs a per-`new` cleanup block (newArgUnwindCleanup) as the
// unwind target ONLY while the ctor arguments are evaluated. An arg-eval throw
// routes through it, RAW-freeing the uninitialized block (a plain `free` — NOT
// a dispose: no destructor / owned-field disposal, because no field is
// initialized) and continuing to the enclosing unwind. The target is restored
// before the constructor body/call runs, whose own cleanup (inlineCtorBody /
// emitNewPendingCheck) then owns the block's disposal — the two edges are
// disjoint, so a normally-constructed object is NEVER raw-freed (no double-free,
// no premature free) and flows out as a live owned value disposed by its slot.
//
// The already-evaluated owned arguments (a bare `owned x` transferred into an
// `owned` ctor param, or an owned `new`-temp argument) are handled by the
// pre-existing buildOrderedArgs machinery (#993 unwind-only temp disposal /
// #1918 deferred transfer-slot nulling): on a later-arg throw the caller's slot
// is still live, so the value is disposed exactly once by the enclosing unwind
// — this fix adds ONLY the missing raw-free of the instance block itself.
//
// Tests (each uses runAotChecked → generated code linked + run under ASan/LSan,
// so a leak, double-free, or use-after-free FAILS via r.ran=false):
//   T1 — plain `new C(1, MakeIntOrThrow())` (no owned arg): arg-throw is LSan-
//        clean, exception caught, rc=0.  (was a 24-byte LSan leak before the fix)
//   T2 — `new Pair(move x, MakeIntOrThrow())`: BOTH the raw instance block is
//        freed (this fix) AND the transferred owned `x` is disposed EXACTLY ONCE
//        (pre-existing buildOrderedArgs machinery) — proven by LSan-cleanliness
//        (a leak = not disposed; a double-free = disposed twice).  (was a 32-byte
//        LSan leak of the Pair block before the fix.)
//   T3 — successful `new Pair(move x, 7)`: constructed + disposed normally, no
//        double-free / no premature free (X disposed once at scope end), rc=0.
//   T4 — generic `new G<Cell>(1, MakeIntOrThrow())` (inline-ctor path): arg-throw
//        LSan-clean, rc=0 (regression guard for the generic construction path).
//   T5 — FIRST argument throws `new C(MakeIntOrThrow(), 2)`: instance freed on
//        the immediate arg-throw, LSan-clean, rc=0.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic. clang is required to
// build this repo, so a skip is not expected — guard anyway (mirrors #1934).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// T1 — the exact #1968 repro: a LATER ctor argument throws; the calloc'd `C` is
// reclaimed on the arg-eval unwind (no owned transfer needed to reproduce).
const char* kPlainLaterArgThrows =
    "module M; "
    "class C "
    "  public _a: int; "
    "  public _b: int; "
    "  public constructor(a: int, b: int) _a := a; _b := b; end constructor; "
    "  destructor end destructor; "
    "end C; "
    "class P "
    "  public static method MakeIntOrThrow(): int "
    "    throw new Exception(\"boom\"); "
    "  end MakeIntOrThrow; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    try using var c := new C(1, P.MakeIntOrThrow()); "
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    return 0; "
    "  end Main; "
    "end P;";

// T2 — owned arg + throwing LATER arg (the issue's `new Pair(owned x, ...)`
// repro): the raw `Pair` block is freed on the arg-eval unwind (this fix) AND
// the already-transferred owned `x` is disposed EXACTLY once by the pre-existing
// buildOrderedArgs machinery. Exactly-once is proven by ASan/LSan cleanliness:
// a leak means `x` (or the Pair block) was NOT disposed, a double-free means it
// was disposed twice — either fails r.ran. (The disposal happens at Main's
// scope-end, AFTER any in-Main check could observe it, so a static-count read
// here would not be a valid witness — LSan is.)
const char* kOwnedArgThenLaterArgThrows =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class Pair "
    "  public owned _x: X; "
    "  public _b: int; "
    "  public constructor(owned x: X, b: int) _x := x; _b := b; end constructor; "
    "  destructor end destructor; "
    "end Pair; "
    "class P "
    "  public static method MakeIntOrThrow(): int "
    "    throw new Exception(\"boom\"); "
    "  end MakeIntOrThrow; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    var x := new X(); "
    "    try using var p := new Pair(move x, P.MakeIntOrThrow()); "
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    return 0; "
    "  end Main; "
    "end P;";

// T3 — SUCCESSFUL construction (no throw): the object is constructed and
// disposed normally; the arg-throw cleanup must NOT raw-free it (no double-free,
// no premature free). `x` transfers into `_x`, disposed exactly once at p's
// scope end → X._n==1.
const char* kSuccessfulConstruction =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class Pair "
    "  public owned _x: X; "
    "  public _b: int; "
    "  public constructor(owned x: X, b: int) _x := x; _b := b; end constructor; "
    "  destructor end destructor; "
    "end Pair; "
    "class P "
    "  public static method Ident(v: int): int return v; end Ident; "
    "  public static method Main(): int "
    "    var x := new X(); "
    "    do "
    "      using var p := new Pair(move x, P.Ident(7)); "
    "      if p._b <> 7 then return 4; end; "
    "    end; "                                 // p disposed here → X._n==1
    "    if X._n <> 1 then return 5; end; "
    "    return 0; "
    "  end Main; "
    "end P;";

// T4 — generic construction (inline-ctor path) with a throwing LATER arg.
const char* kGenericLaterArgThrows =
    "module M; "
    "class G<T> where T: class "
    "  public _a: int; "
    "  public constructor(a: int, b: int) _a := a + b; end constructor; "
    "  destructor end destructor; "
    "end G; "
    "class Cell end Cell; "
    "class P "
    "  public static method MakeIntOrThrow(): int "
    "    throw new Exception(\"boom\"); "
    "  end MakeIntOrThrow; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    try using var g := new G<Cell>(1, P.MakeIntOrThrow()); "
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    return 0; "
    "  end Main; "
    "end P;";

// T5 — the FIRST argument throws: the instance is freed on the immediate
// arg-throw unwind (nothing was evaluated into it yet).
const char* kFirstArgThrows =
    "module M; "
    "class C "
    "  public _a: int; "
    "  public _b: int; "
    "  public constructor(a: int, b: int) _a := a; _b := b; end constructor; "
    "  destructor end destructor; "
    "end C; "
    "class P "
    "  public static method MakeIntOrThrow(): int "
    "    throw new Exception(\"boom\"); "
    "  end MakeIntOrThrow; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    try using var c := new C(P.MakeIntOrThrow(), 2); "
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    return 0; "
    "  end Main; "
    "end P;";

}  // namespace

// ── T1: §28.3 — a later ctor argument that throws frees the calloc'd block ────
BEZEL_TEST(new_ctor_arg_throw_leak_1968, plain_later_arg_throws_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kPlainLaterArgThrows);
    if (wasSkipped(r)) return;   // ASan toolchain unavailable — skip, not a fail
    BEZEL_EXPECT_TRUE(r.ran);        // compiled + LSan-clean (obj block freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // exception caught
}

// ── T2: §28.3 — raw block freed AND transferred owned arg disposed once ───────
BEZEL_TEST(new_ctor_arg_throw_leak_1968, owned_arg_plus_later_throw_disposed_once_spec_28_3) {
    JitResult r = runAotChecked(kOwnedArgThenLaterArgThrows);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean: Pair block freed (this fix), owned x disposed once
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // exception caught
}

// ── T3: §28.3 — successful construction: no double-free / no premature free ───
BEZEL_TEST(new_ctor_arg_throw_leak_1968, successful_construction_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(kSuccessfulConstruction);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean: object constructed + disposed normally
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // _b==7 AND X._n==1 at scope end
}

// ── T4: §28.3/§14 — generic inline-ctor path, later arg throws ────────────────
BEZEL_TEST(new_ctor_arg_throw_leak_1968, generic_later_arg_throws_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kGenericLaterArgThrows);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean on the generic construction path
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── T5: §28.3 — the first argument throws: instance freed immediately ─────────
BEZEL_TEST(new_ctor_arg_throw_leak_1968, first_arg_throws_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kFirstArgThrows);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean (obj block freed on the first-arg throw)
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
