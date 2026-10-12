// Codegen / refcounting — issue #2817: EVERY `var` string local declared in an
// ASYNC ITERATOR body, a SYNC ITERATOR body, a plain LAMBDA body, an ASYNC
// LAMBDA body, or a COMPILED CONSTRUCTOR body leaked its reference. Two
// allocations (the `bz_string` header plus its data buffer) per string per call,
// unboundedly in a service loop. The five siblings #2807 fixed for the plain-
// async and generic-specialization bodies, and deliberately stopped short of.
//
// Spec: §4.6 "`string`"; §28.9 "Runtime-managed types" — "`string` — pervasive;
// created by literals, interpolation, concatenation, and library methods", and
// "The implementation of runtime-managed types' lifecycle (reference counting,
// region allocation, or other) is not part of this specification"; §28.3 "Local
// variables, ownership, and borrows"; §28.1 "Where the `owned` modifier can
// appear" for the parameter-borrow rule the co-gated retain rests on; §29.2
// "Writing an iterator" and §29.7 "Iterator disposal and `finally`" for the
// synchronous iterator's advance / abandonment model; §30.7 "Async iterators"
// for its asynchronous twin; §27.3 "Lambda expressions" / §27.5 "Captured
// variables" for the lambda bodies; §30.6 "`async` lambdas"; §22.8 "Construction
// sequence" for the constructor body's step ordering. Spec revision v0.240.
//
// None of these locals carries a §28 disposal obligation (§28.9 makes `string`
// runtime-managed and §28.1 makes `owned` on one a compile error). What each
// carries, in THIS implementation, is one reference, and the observable
// requirement is that the heap string be reclaimed. It was not.
//
// THE DEFECT. Every consumer of the three string-eligibility sets gates on
// `currentFn_ == stringReleaseEligibleFn_`: armStringLocalRefcount (the sole
// registrar of the bind-time retain AND the scope-end Disposal{stringRelease}),
// returnedStringLocalOwnsPlusOne, the two record-backed interface-property
// retains, and the `:=` / §4.6 `+=` drop-on-overwrite release. After #2807 that
// token was pinned in exactly three places — emitFunctionBody, emitAsyncMethod's
// resume prolog, buildSpecializedMethodFn — and NONE of them is reached for an
// iterator MoveNext/Resume, a lambda fn, an async-lambda Resume, or a compiled
// constructor. All five lowered with `currentFn_` naming a fresh llvm::Function
// while the token still named some other body, so the gate was false for every
// string local in them.
//
// THE FIX runs collectStringReleaseEligible for the body being emitted and pins
// the token to the function that lowers it, in all five places — the same
// re-establishment those sites already perform for the unwind (#1896/#1897),
// closure (#422/#488), capture-cell (#1416/#1418) and borrowing-collection
// (#2132/#2142/#2143/#2148) analyses.
//
// THE THREE THINGS THAT CARRY THE SAFETY ARGUMENT, each verified by a control
// experiment (comment the mechanism out, rebuild, re-run):
//
//   * CO-GATED PARAMETER RETAIN. Pinning the token also enables the drop-on-
//     overwrite release for a `string` PARAMETER (localSlot() resolves one),
//     whose single reference belongs to the CALLER — §28.1: on a `string`,
//     "`owned` is a compile error: such values carry no disposal obligation to
//     transfer (§28.9)". Releasing it unpaired is the #2696 use-after-free, so
//     retainAssignedStringParams is called in the same place. PROVEN load-bearing
//     for all five: removing only that call turns cases (K)(L)(M)(N)(O) into
//     "AddressSanitizer: heap-use-after-free" (measured, one body kind at a
//     time). In an iterator MoveNext / async resume the retain sits in the entry
//     block, which dominates every state block and re-runs per advance, while a
//     given advance releases the incoming value at most once — so retains >=
//     releases on every path.
//
//   * SAVE / RESTORE around a lambda. A lambda body is lowered MID-EMISSION of
//     its enclosing function, so lowerLambda saves and restores the three sets
//     and the token around the whole emission (both the sync pin and
//     emitAsyncLambda's resume pin). PROVEN load-bearing: dropping only the
//     restore makes (P) and (Q) leak 934 B / 40 where the fix leaves them clean
//     — WORSE than the 371 B / 20 they leaked before the fix, because the
//     enclosing body's own locals then lose their releases too.
//
//   * INLINED-CONSTRUCTOR ANALYSIS. inlineCtorBody lowers a generic class's
//     constructor into the ENCLOSING function, so the release gate is about that
//     enclosing function while the eligibility sets describe its AST — the
//     inlined body's own LocalDecls are in none of them. A BORROW-initialized
//     local there took no bind-time retain and then had its first assignment
//     release the reference it merely borrowed: case (T), a heap-use-after-free
//     reproducible on the base of this change. The constructor pin would have
//     EXTENDED that hole to a compiled constructor chain-calling a generic base
//     (case (U): a 1324 B / 60 bounded leak became a heap-use-after-free), so
//     inlineCtorBody now runs the analysis for the body it is actually lowering
//     and restores the enclosing body's state afterwards. Gated on the enclosing
//     body ALREADY being the analyzed one: where the release does not fire there
//     is nothing to pair, and arming one would be the new use-after-free.
//
// (T) and (U) are therefore the two cases that pin the USE-AFTER-FREE direction
// of this change; (F)-(J) are exactly-once oracles rather than mere leak checks.
//
// ⚠ Every string here is built at RUNTIME (`$"it-{i}"`): `"ab" + "cd"` constant-
// folds to an rc sentinel on which bz_string_release no-ops — the trap that has
// made probes in this family vacuous repeatedly. That is also why the `Holder`
// field is assigned `$"hn-{seed}"` from a variable rather than given a literal
// initializer.
//
// ⚠ EVERY case is LOOP-AMPLIFIED and allocates a churn string per iteration. A
// ONE-call `Main` is NOT a reliable LSan oracle (a leftover pointer in a register
// makes LSan report a leaked block as reachable, so the same binary reports a
// leak on some runs and not others).
//
// MEASURED, per case, `--sanitize=address`, pre-fix -> post-fix, with the PLAIN
// (unsanitized) value identical on both sides in every case:
//   (A) async iterator locals      1904 B / 80  -> clean       (91)
//   (B) sync iterator locals       1845 B / 78  -> clean       (91)
//   (C) lambda locals              1904 B / 80  -> clean       (91)
//   (D) async lambda locals        1904 B / 80  -> clean       (91)
//   (E) compiled ctor locals       1904 B / 80  -> clean      (111)
//   (F)-(J) exactly-once oracles   1904..2000   -> clean       (84)
//   (P) lambda save/restore         371 B / 20  -> clean      (123)
//   (Q) async-lambda save/restore   371 B / 20  -> clean      (123)
//   (R) sync iterator abandonment  5580 B / 240 -> clean      (240)
//   (S) async iterator abandonment 5580 B / 240 -> clean      (240)
//   (T) inlined generic ctor       heap-use-after-free -> clean (100)
//   (U) compiled ctor -> generic base   1324 B / 60 -> clean  (141)
//   (V) lambda returns its local   1904 B / 80  -> clean       (91)
//   (W) LINQ block-bodied lambda   4700 B / 200 -> clean      (250)
//   (X) constructor throw edges    1904 B / 80  -> clean       (84)
//   (K) lambda `string` param      unchanged leak, no memory error (100)
//   (L) async lambda param         unchanged leak, no memory error (100)
//   (M) sync iterator param        unchanged leak, no memory error  (64)
//   (N) async iterator param       unchanged leak, no memory error  (64)
//   (O) compiled ctor param        unchanged leak, no memory error (100)
// (K)-(O) still leak the parameter slot's FINAL value — #2696's documented,
// deliberate imbalance ("No scope-end release is registered for the parameter
// slot, so its final value leaks") — and no probe reports a use-after-free,
// double-free or SEGV.
//
// KNOWN RESIDUALS, measured post-fix and filed separately, each a bounded leak
// with the correct value and no memory error: a `finally` body REPLAYED by an
// iterator's Dispose is lowered in the Dispose fn, which no pin names, so its own
// string locals still leak (380 B / 20 for a 10x-abandoned iterator, down from
// 5112 B / 222); and a generic specialization does not re-establish the CLOSURE
// release eligibility, so a `var f := <lambda>` local inside one leaks its
// 40-byte closure cell (400 B / 10, down from 771 B / 30).
//
// #3733 — THE TOOLCHAIN SKIP IS NOW LOUD, NOT ABSORBED. Every `runAotChecked`
// row here used to read
//
//     if (wasSkipped(r)) return;
//
// where `wasSkipped` matched the literal "SKIP" and nothing else. On a box whose
// ASan/LSan link toolchain is absent, that branch RETURNS BEFORE ANY ASSERTION
// runs, so the row reports a clean PASS having executed no sanitizer at all —
// the #3733 silent-skip class, and the worst failure mode this harness has,
// because it fails silently and only on the machine that cannot check. This file
// is the single largest holder of that shape: 25 absorbing sites, more than any
// other file in the tree, and counted by
// scripts/check_toolchain_skip_surface.py.
//
// Each guard is replaced by the loud, positive form shared across the burn-down
// (tests/support/aot_result_asserts.hpp::checkedToolchainSkipped, the idiom
// fixes #5105 / #5145 / #5410 / #2080 landed first):
//
//     BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
//
// The row now still passes where the toolchain is genuinely absent — a
// diagnostic naming "SKIP" is the ONLY thing this predicate reports — but it
// FAILS anywhere a real sanitizer finding was being discarded as though the box
// merely lacked clang. That is the strict strengthening: no expectation below
// any of these 25 rows was relaxed, and the file-local `wasSkipped` predicate is
// deleted rather than left behind as an unused SKIP reader. The predicate must
// stay a SEPARATE statement from any `if` over `r`; inlining it back into a
// guard re-creates exactly the absorbing shape this line replaces.
//
// MEASURED on the tree this landed on: all 25 rows were already genuinely
// passing, not passing-only-because-of-the-guard — `CHECKED TOOLCHAIN: 0
// skip(s)` is printed unconditionally by tests/test_main.cpp and increments at
// the single chokepoint that can emit the SKIP diagnostic
// (tests/support/aot_support.hpp runAotCheckedImpl), so a 0 there proves no
// guard fired and every assertion below it really ran. 44 passed / 0 failed
// before and after the change.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string sanitizerText(const JitResult& r) {
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    return all;
}

// A checked run that is expected to LEAK (a documented, pre-existing trade) and,
// in particular, NOT to have freed anything early. `r.ran` is false for a leak
// AND for a use-after-free, so the distinction has to be read out of the report.
void expectLeakButNoMemoryError(const JitResult& r) {
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") != std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("SEGV") == std::string::npos);
}

// sum over i in 1..20 of len("xx-{i}") = 9*4 + 11*5 = 91.
constexpr int kLenSum20 = 91;

// The churn allocation every case makes. The `< 0` test is never true; it forces
// the read so the local cannot be elided, and the allocate/free pair means a
// prematurely freed block cannot be read back intact on the plain path.
#define CHURN                                                                  \
    "    var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "                    \
    "    if churn.Length < 0 then return 99; end; "
// The `yield`-body form: an iterator body cannot `return` a value (§29.2).
#define CHURN_Y                                                                \
    "    var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "                    \
    "    if churn.Length < 0 then yield return 99; end; "
// The lambda form (its loop variable is `n`).
#define CHURN_N                                                                \
    "    var churn := $\"churn-churn-churn-{n}-{n}-{n}\"; "                    \
    "    if churn.Length < 0 then return 99; end; "

// A `Holder` whose `string` field is filled with a RUNTIME-built value (a
// literal initializer would be an rc sentinel and make the oracle vacuous).
#define HOLDER_DECL                                                            \
    "module M; class Holder "                                                  \
    "  public Name: string := \"\"; "                                          \
    "end Holder; "
#define HOLDER_INIT                                                            \
    "    using var h := new Holder(); "                                        \
    "    var seed := 7; "                                                      \
    "    h.Name := $\"hn-{seed}\"; "        /* len 4 */

// =====================================================================
// (A) ASYNC ITERATOR body — the issue's first repro. §30.7 / §4.6 / §28.9.
// =====================================================================
const char* kAsyncIteratorLocals =
    "module M; class C "
    "  private static async method Seq(n: int): IAsyncEnumerable<int> "
    "    for var i in 1..n do "
    "      var built := $\"it-{i}\"; "
    CHURN_Y
    "      yield return built.Length; "
    "    end; "
    "  end Seq; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    await for var v in C.Seq(20) do "
    "      total := total + v; "
    "    end; "
    "    return total; "                     // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (B) SYNC ITERATOR body. §29.2 / §4.6 / §28.9.
// =====================================================================
const char* kSyncIteratorLocals =
    "module M; class C "
    "  private static method Seq(n: int): IEnumerable<int> "
    "    for var i in 1..n do "
    "      var built := $\"it-{i}\"; "
    CHURN_Y
    "      yield return built.Length; "
    "    end; "
    "  end Seq; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var v in C.Seq(20) do "
    "      total := total + v; "
    "    end; "
    "    return total; "                     // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (C) plain LAMBDA body — the case armStringLocalRefcount's own comment named
// ("a lambda/thunk body lowered mid-function is not walked"). §27.3.
// =====================================================================
const char* kLambdaLocals =
    "module M; class C "
    "  public static method Main(): int "
    "    var f := lambda(n: int): int => do "
    "      var built := $\"lm-{n}\"; "
    CHURN_N
    "      return built.Length; "
    "    end; "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + f(i); "
    "    end; "
    "    return total; "                     // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (D) ASYNC LAMBDA body. §30.6.
// =====================================================================
const char* kAsyncLambdaLocals =
    "module M; class C "
    "  public static async method Main(): Task<int> "
    "    var f := async lambda(n: int): Task<int> => do "
    "      var built := $\"lm-{n}\"; "
    CHURN_N
    "      return built.Length; "
    "    end; "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + await f(i); "
    "    end; "
    "    return total; "                     // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (E) COMPILED CONSTRUCTOR body — the case LlvmBackend_Setup.cpp recorded as a
// deliberate no-op ("A COMPILED constructor never sets
// stringReleaseEligibleFn_"). §22.8.
// =====================================================================
const char* kCtorLocals =
    "module M; class Maker "
    "  public Name: string := \"\"; "
    "  public constructor(i: int) "
    "    var built := $\"ct-{i}\"; "
    "    var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "    if churn.Length < 0 then Name := \"x\"; end; "
    "    Name := built + \"!\"; "
    "  end constructor; "
    "end Maker; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var m := new Maker(i); "
    "      total := total + m.Name.Length; "
    "    end; "
    "    return total; "                     // 111
    "  end Main; "
    "end C;";

// =====================================================================
// (F)-(J) EXACTLY-ONCE ORACLES, one per body kind. A BORROW local is retained at
// bind and released at scope end against the SAME heap string a live `Holder`
// field owns, 20 times, and the field is read again afterwards. An over-release
// frees it under the object — and the generated code is ASan-instrumented, so
// the read faults as heap-use-after-free; an under-release leaks it past the
// Holder's disposal (LSan). Both directions are observable in one program.
// §28.3 / §28.9.
// =====================================================================
const char* kLambdaBorrowOnce =
    HOLDER_DECL
    "class C "
    "  public static method Main(): int "
    HOLDER_INIT
    "    var f := lambda(hh: Holder, n: int): int => do "
    "      var borrowed := hh.Name; "
    CHURN_N
    "      return borrowed.Length; "
    "    end; "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + f(h, i); "
    "    end; "
    "    return total + h.Name.Length; "     // 20*4 + 4 = 84
    "  end Main; "
    "end C;";

const char* kAsyncLambdaBorrowOnce =
    HOLDER_DECL
    "class C "
    "  public static async method Main(): Task<int> "
    HOLDER_INIT
    "    var f := async lambda(hh: Holder, n: int): Task<int> => do "
    "      var borrowed := hh.Name; "
    CHURN_N
    "      return borrowed.Length; "
    "    end; "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + await f(h, i); "
    "    end; "
    "    return total + h.Name.Length; "     // 84
    "  end Main; "
    "end C;";

const char* kSyncIteratorBorrowOnce =
    HOLDER_DECL
    "class C "
    "  private static method Seq(h: Holder, n: int): IEnumerable<int> "
    "    for var i in 1..n do "
    "      var borrowed := h.Name; "
    CHURN_Y
    "      yield return borrowed.Length; "
    "    end; "
    "  end Seq; "
    "  public static method Main(): int "
    HOLDER_INIT
    "    var total := 0; "
    "    for var v in C.Seq(h, 20) do "
    "      total := total + v; "
    "    end; "
    "    return total + h.Name.Length; "     // 84
    "  end Main; "
    "end C;";

const char* kAsyncIteratorBorrowOnce =
    HOLDER_DECL
    "class C "
    "  private static async method Seq(h: Holder, n: int): IAsyncEnumerable<int> "
    "    for var i in 1..n do "
    "      var borrowed := h.Name; "
    CHURN_Y
    "      yield return borrowed.Length; "
    "    end; "
    "  end Seq; "
    "  public static async method Main(): Task<int> "
    HOLDER_INIT
    "    var total := 0; "
    "    await for var v in C.Seq(h, 20) do "
    "      total := total + v; "
    "    end; "
    "    return total + h.Name.Length; "     // 84
    "  end Main; "
    "end C;";

const char* kCtorBorrowOnce =
    HOLDER_DECL
    "class Peek "
    "  public Len: int := 0; "
    "  public constructor(h: Holder, i: int) "
    "    var borrowed := h.Name; "
    "    var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "    if churn.Length < 0 then Len := 99; end; "
    "    Len := borrowed.Length; "
    "  end constructor; "
    "end Peek; "
    "class C "
    "  public static method Main(): int "
    HOLDER_INIT
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var p := new Peek(h, i); "
    "      total := total + p.Len; "
    "    end; "
    "    return total + h.Name.Length; "     // 84
    "  end Main; "
    "end C;";

// =====================================================================
// (K)-(O) THE USE-AFTER-FREE DIRECTION — the body ASSIGNS to its `string`
// PARAMETER, which the pin turns into a drop-on-overwrite release of a reference
// the CALLER owns (§28.1). The co-gated bind-time retain is what keeps the
// caller's string alive across the round trips; the caller reads it again after
// churn each time. Removing only that retain (per body kind) makes each of these
// report heap-use-after-free — measured.
// =====================================================================
const char* kLambdaAssignedParam =
    HOLDER_DECL
    "class C "
    "  public static method Main(): int "
    HOLDER_INIT
    "    var f := lambda(s: string, n: int): int => do "
    "      s := s + \"-x\"; "
    CHURN_N
    "      return s.Length; "
    "    end; "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + f(h.Name, i); "
    "      var churn2 := $\"more-churn-{i}-{i}-{i}\"; "
    "      if churn2.Length < 0 then return 98; end; "
    "      total := total + h.Name.Length; "
    "    end; "
    "    return total; "                     // 10*6 + 10*4 = 100
    "  end Main; "
    "end C;";

const char* kAsyncLambdaAssignedParam =
    HOLDER_DECL
    "class C "
    "  public static async method Main(): Task<int> "
    HOLDER_INIT
    "    var f := async lambda(s: string, n: int): Task<int> => do "
    "      s := s + \"-x\"; "
    CHURN_N
    "      return s.Length; "
    "    end; "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + await f(h.Name, i); "
    "      var churn2 := $\"more-churn-{i}-{i}-{i}\"; "
    "      if churn2.Length < 0 then return 98; end; "
    "      total := total + h.Name.Length; "
    "    end; "
    "    return total; "                     // 100
    "  end Main; "
    "end C;";

const char* kSyncIteratorAssignedParam =
    HOLDER_DECL
    "class C "
    "  private static method Seq(s: string, n: int): IEnumerable<int> "
    "    for var i in 1..n do "
    "      s := s + \"-x\"; "
    CHURN_Y
    "      yield return s.Length; "
    "    end; "
    "  end Seq; "
    "  public static method Main(): int "
    HOLDER_INIT
    "    var total := 0; "
    "    for var v in C.Seq(h.Name, 10) do "
    "      total := total + v; "
    "      var churn2 := $\"more-churn-{v}-{v}-{v}\"; "
    "      if churn2.Length < 0 then return 98; end; "
    "    end; "
    "    return total + h.Name.Length; "     // 10*6 + 4 = 64
    "  end Main; "
    "end C;";

const char* kAsyncIteratorAssignedParam =
    HOLDER_DECL
    "class C "
    "  private static async method Seq(s: string, n: int): IAsyncEnumerable<int> "
    "    for var i in 1..n do "
    "      s := s + \"-x\"; "
    CHURN_Y
    "      yield return s.Length; "
    "    end; "
    "  end Seq; "
    "  public static async method Main(): Task<int> "
    HOLDER_INIT
    "    var total := 0; "
    "    await for var v in C.Seq(h.Name, 10) do "
    "      total := total + v; "
    "      var churn2 := $\"more-churn-{v}-{v}-{v}\"; "
    "      if churn2.Length < 0 then return 98; end; "
    "    end; "
    "    return total + h.Name.Length; "     // 64
    "  end Main; "
    "end C;";

const char* kCtorAssignedParam =
    HOLDER_DECL
    "class Take "
    "  public Len: int := 0; "
    "  public constructor(s: string, i: int) "
    "    s := s + \"-x\"; "
    "    var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "    if churn.Length < 0 then Len := 98; end; "
    "    Len := s.Length; "
    "  end constructor; "
    "end Take; "
    "class C "
    "  public static method Main(): int "
    HOLDER_INIT
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      using var t := new Take(h.Name, i); "
    "      total := total + t.Len; "
    "      var churn2 := $\"more-churn-{i}-{i}-{i}\"; "
    "      if churn2.Length < 0 then return 98; end; "
    "      total := total + h.Name.Length; "
    "    end; "
    "    return total; "                     // 100
    "  end Main; "
    "end C;";

// =====================================================================
// (P) / (Q) THE SAVE/RESTORE GUARD. A lambda is lowered MID-EMISSION of the body
// that builds its closure. That body has its own armed string locals declared
// BOTH BEFORE and AFTER the lambda: if lowerLambda left the sets / fn token
// pointing at the lambda, every local after it would silently lose its release.
// Pre-fix these leaked exactly the lambda's own local (371 B / 20); dropping only
// the restore makes them leak 934 B / 40 — worse than pre-fix. §27.3 / §30.6.
// =====================================================================
const char* kLambdaSaveRestore =
    "module M; class C "
    "  private static method Outer(i: int): int "
    "    var before := $\"be-{i}\"; "
    "    var f := lambda(n: int): int => do "
    "      var inner := $\"in-{n}\"; "
    "      return inner.Length; "
    "    end; "
    "    var mid := f(i); "
    "    var after := $\"af-{i}\"; "
    CHURN
    "    return before.Length + mid + after.Length; "
    "  end Outer; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + C.Outer(i); "
    "    end; "
    "    return total; "                     // 3 * 41 = 123
    "  end Main; "
    "end C;";

const char* kAsyncLambdaSaveRestore =
    "module M; class C "
    "  private static async method Outer(i: int): Task<int> "
    "    var before := $\"be-{i}\"; "
    "    var f := async lambda(n: int): Task<int> => do "
    "      var inner := $\"in-{n}\"; "
    "      return inner.Length; "
    "    end; "
    "    var mid := await f(i); "
    "    var after := $\"af-{i}\"; "
    CHURN
    "    return before.Length + mid + after.Length; "
    "  end Outer; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + await C.Outer(i); "
    "    end; "
    "    return total; "                     // 123
    "  end Main; "
    "end C;";

// =====================================================================
// (R) / (S) ABANDONMENT. The consumer `break`s after 3 of 50 elements, 20 times,
// so the iterator is Disposed while SUSPENDED at a yield and the scope-end
// release inside MoveNext is never reached. The yield handler snapshots the
// disposals live at that suspend (smIterCleanupAtState_, issue 3579 — a
// Disposal{stringRelease} is neither `caughtException` nor `unwindOnly`, and
// its slot is in locals_, so it IS recorded) and Dispose replays them with the
// frame locals restored. Exactly once on either exit: Dispose stamps the DISPOSED
// sentinel so no later MoveNext can resume past the yield. §29.7 / §30.7.
// =====================================================================
const char* kSyncIteratorAbandoned =
    "module M; class C "
    "  private static method Seq(n: int): IEnumerable<int> "
    "    for var i in 1..n do "
    "      var built := $\"it-{i}\"; "
    CHURN_Y
    "      yield return built.Length; "
    "    end; "
    "  end Seq; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var r in 1..20 do "
    "      var seen := 0; "
    "      for var v in C.Seq(50) do "
    "        total := total + v; "
    "        seen := seen + 1; "
    "        if seen >= 3 then break; end; "
    "      end; "
    "      var churn2 := $\"more-churn-{r}-{r}-{r}\"; "
    "      if churn2.Length < 0 then return 98; end; "
    "    end; "
    "    return total; "                     // 20 * (4+4+4) = 240
    "  end Main; "
    "end C;";

const char* kAsyncIteratorAbandoned =
    "module M; class C "
    "  private static async method Seq(n: int): IAsyncEnumerable<int> "
    "    for var i in 1..n do "
    "      var built := $\"it-{i}\"; "
    CHURN_Y
    "      yield return built.Length; "
    "    end; "
    "  end Seq; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var r in 1..20 do "
    "      var seen := 0; "
    "      await for var v in C.Seq(50) do "
    "        total := total + v; "
    "        seen := seen + 1; "
    "        if seen >= 3 then break; end; "
    "      end; "
    "      var churn2 := $\"more-churn-{r}-{r}-{r}\"; "
    "      if churn2.Length < 0 then return 98; end; "
    "    end; "
    "    return total; "                     // 240
    "  end Main; "
    "end C;";

// =====================================================================
// (T) THE PRE-EXISTING USE-AFTER-FREE this change closes. A generic class's
// constructor is INLINED into the enclosing function (inlineCtorBody, §12/§14),
// so the release gate is about that function while the eligibility sets describe
// ITS ast — the inlined body's `var s := h.Name;` is in none of them, takes no
// bind-time retain, and its `s := s + "-x";` releases the Holder's reference.
// Reproduced on the base of this change as heap-use-after-free.
// =====================================================================
const char* kInlinedGenericCtorBorrowThenAssign =
    HOLDER_DECL
    "class Box<T> "
    "  public Len: int := 0; "
    "  public constructor(h: Holder, i: int) "
    "    var s := h.Name; "
    "    s := s + \"-x\"; "
    "    Len := s.Length; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    HOLDER_INIT
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      using var b := new Box<int>(h, i); "
    "      total := total + b.Len; "
    "      var churn := $\"more-churn-{i}-{i}-{i}\"; "
    "      if churn.Length < 0 then return 98; end; "
    "      total := total + h.Name.Length; "
    "    end; "
    "    return total; "                     // 10*6 + 10*4 = 100
    "  end Main; "
    "end C;";

// =====================================================================
// (U) THE REGRESSION THE CONSTRUCTOR PIN WOULD HAVE INTRODUCED. A COMPILED
// constructor `base(...)`-chains a GENERIC base whose body is inlined into it.
// Before the inlineCtorBody analysis was added, pinning the compiled ctor turned
// this from a 1324 B / 60 bounded leak into a heap-use-after-free. §22.9 step 1.
// =====================================================================
const char* kCompiledCtorInlinesGenericBase =
    HOLDER_DECL
    "class BaseG<T> "
    "  public Len: int := 0; "
    "  public constructor(h: Holder) "
    "    var s := h.Name; "
    "    s := s + \"-x\"; "
    "    Len := s.Length; "
    "  end constructor; "
    "end BaseG; "
    "class Concrete : BaseG<int> "
    "  public constructor(h: Holder, i: int) "
    "    base(h); "
    "    var d := $\"dv-{i}\"; "
    "    var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "    if churn.Length < 0 then Len := 98; end; "
    "    Len := Len + d.Length; "
    "  end constructor; "
    "end Concrete; "
    "class C "
    "  public static method Main(): int "
    HOLDER_INIT
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      using var c := new Concrete(h, i); "
    "      total := total + c.Len; "
    "      var churn2 := $\"more-churn-{i}-{i}-{i}\"; "
    "      if churn2.Length < 0 then return 98; end; "
    "      total := total + h.Name.Length; "
    "    end; "
    "    return total; "                     // 9*10 + 11 + 10*4 = 141
    "  end Main; "
    "end C;";

// =====================================================================
// (V) A lambda BLOCK body that RETURNS its own string local. The pin also turns
// on returnedStringLocalOwnsPlusOne for the lambda, so the local's single
// reference is TRANSFERRED to the caller instead of a second one being minted;
// the escape walk keeps the scope-end release off, so it is not released twice.
// The caller reads every returned value. §27.3 / §4.6.
// =====================================================================
const char* kLambdaReturnsItsLocal =
    "module M; class C "
    "  public static method Main(): int "
    "    var f := lambda(n: int): string => do "
    "      var built := $\"lm-{n}\"; "
    "      var churn := $\"churn-churn-churn-{n}-{n}-{n}\"; "
    "      if churn.Length < 0 then return \"x\"; end; "
    "      return built; "
    "    end; "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var got := f(i); "
    "      total := total + got.Length; "
    "    end; "
    "    return total; "                     // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (W) The everyday shape: a block-bodied lambda handed to a LINQ operator.
// stdlib §5.1 `Select` / `ToArray`. §27.3.
// =====================================================================
const char* kLinqBlockLambda =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var r in 1..10 do "
    "      var xs: Array<int> := [1, 2, 3, 4, 5]; "
    "      var ys := xs.Select(lambda(n: int): int => do "
    "        var built := $\"sel-{n}\"; "
    CHURN_N
    "        return built.Length; "
    "      end).ToArray(); "
    "      for var y in ys do "
    "        total := total + y; "
    "      end; "
    "    end; "
    "    return total; "                     // 10 * 5 * 5 = 250
    "  end Main; "
    "end C;";

// =====================================================================
// (Y) THE DOUBLE FREE THIS CHANGE ALMOST INTRODUCED, pinned. Pinning the token
// to a LAMBDA body makes returnedStringLocalOwnsPlusOne run there for the first
// time — and its producer branch re-derives "+1 producer initializer,
// `string`-typed" straight from the decl, so a `return s;` naming a CAPTURED
// enclosing local (`var s := p + "!";`, §27.5) read as "this body owns the +1"
// and SKIPPED the return retain. The caller then released a +0 borrow: a double
// free. The capture's reference is co-owned through the shared cell and dropped
// by the env destructor — the lambda body owns nothing of it. stringLocalCandidates_
// (the decls the analysis actually admitted for THIS body) is the guard; removing
// only that test reproduces the failure here and in the #2689 / #2706 suites.
// §27.5 / §28.9.
// =====================================================================
const char* kLambdaReturnsCapturedLocal =
    "module M; class C "
    "  public static method Two(p: string): int "
    "    var s := p + \"!\"; "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    var f2 := lambda(k: int) => do return s; end; "
    "    var churn := $\"churn-churn-churn-{p}\"; "
    "    if churn.Length < 0 then return 99; end; "
    "    return f1(1).Length + f2(2).Length + s.Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..9 do "
    "      total := total + C.Two($\"v-{i}\"); "   // runtime-built, NOT folded
    "    end; "
    "    return total; "                           // 9 * (4+4+4) = 108
    "  end Main; "
    "end C;";

// =====================================================================
// (X) The constructor THROW edge. `ctor.cleanup` runs only the owned-field and
// base-subobject disposals — never emitUnwindDisposals — so a body throw leaks
// these locals exactly as it did before this fix (the safe direction). What this
// pins is that the normal-completion path still releases exactly once and the
// throwing path frees nothing early. §22.9 step 3.
// =====================================================================
const char* kCtorThrowEdges =
    "module M; class Maker "
    "  public Name: string := \"\"; "
    "  public constructor(i: int) "
    "    var built := $\"ct-{i}\"; "
    "    var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "    if churn.Length < 0 then Name := \"x\"; end; "
    "    if i mod 3 = 0 then throw new InvalidOperationException(\"boom\"); end; "
    "    Name := built + \"!\"; "
    "  end constructor; "
    "end Maker; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      try "
    "        using var m := new Maker(i); "
    "        total := total + m.Name.Length; "
    "      catch InvalidOperationException => total := total + 1; "
    "      end; "
    "    end; "
    "    return total; "                     // 78 + 6 = 84
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------
// (A)-(E) the five body kinds the issue names.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           async_iterator_body_locals_are_released_spec_30_7) {
    JitResult r = runAotChecked(kAsyncIteratorLocals);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 1904 B / 80 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           async_iterator_body_plain_path_value_invariant_spec_30_7) {
    // NOT a #2817 oracle — a leak is invisible without a sanitizer, so this
    // passes with and without the fix. It pins that nothing is freed early.
    JitResult r = runAot(kAsyncIteratorLocals);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           sync_iterator_body_locals_are_released_spec_29_2) {
    JitResult r = runAotChecked(kSyncIteratorLocals);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 1845 B / 78 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           sync_iterator_body_plain_path_value_invariant_spec_29_2) {
    JitResult r = runAot(kSyncIteratorLocals);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           lambda_body_locals_are_released_spec_27_3) {
    JitResult r = runAotChecked(kLambdaLocals);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 1904 B / 80 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           lambda_body_plain_path_value_invariant_spec_27_3) {
    JitResult r = runAot(kLambdaLocals);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           async_lambda_body_locals_are_released_spec_30_6) {
    JitResult r = runAotChecked(kAsyncLambdaLocals);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 1904 B / 80 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           async_lambda_body_plain_path_value_invariant_spec_30_6) {
    JitResult r = runAot(kAsyncLambdaLocals);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           compiled_constructor_body_locals_are_released_spec_22_8) {
    JitResult r = runAotChecked(kCtorLocals);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 1904 B / 80 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 111);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           compiled_constructor_plain_path_value_invariant_spec_22_8) {
    JitResult r = runAot(kCtorLocals);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 111);
}

// ---------------------------------------------------------------------
// (F)-(J) EXACTLY-ONCE oracles — 20 retain/release cycles against a live
// object's field, then the field is read again. Over-release => the read faults
// under ASan; under-release => LSan reports the leak.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           lambda_borrow_local_releases_exactly_once_spec_28_3) {
    JitResult r = runAotChecked(kLambdaBorrowOnce);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           lambda_borrow_local_field_survives_every_cycle_spec_28_3) {
    JitResult r = runAot(kLambdaBorrowOnce);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           async_lambda_borrow_local_releases_exactly_once_spec_28_3) {
    JitResult r = runAotChecked(kAsyncLambdaBorrowOnce);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           sync_iterator_borrow_local_releases_exactly_once_spec_28_3) {
    JitResult r = runAotChecked(kSyncIteratorBorrowOnce);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           sync_iterator_borrow_field_survives_every_advance_spec_28_3) {
    JitResult r = runAot(kSyncIteratorBorrowOnce);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           async_iterator_borrow_local_releases_exactly_once_spec_28_3) {
    JitResult r = runAotChecked(kAsyncIteratorBorrowOnce);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           constructor_borrow_local_releases_exactly_once_spec_28_3) {
    JitResult r = runAotChecked(kCtorBorrowOnce);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           constructor_borrow_field_survives_every_construction_spec_28_3) {
    JitResult r = runAot(kCtorBorrowOnce);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

// ---------------------------------------------------------------------
// (K)-(O) the use-after-free direction — an ASSIGNED-TO `string` parameter.
// Each still leaks the parameter slot's final value (#2696's documented trade)
// and must NOT have freed the CALLER's reference. Removing the co-gated retain
// for the matching body kind turns each of these into heap-use-after-free
// (verified, one body kind at a time).
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           lambda_assigned_string_param_does_not_free_caller_spec_28_1) {
    JitResult r = runAotChecked(kLambdaAssignedParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectLeakButNoMemoryError(r);         // 390 B / 20
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           lambda_assigned_string_param_caller_value_survives_spec_28_1) {
    JitResult r = runAot(kLambdaAssignedParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           async_lambda_assigned_string_param_does_not_free_caller_spec_28_1) {
    JitResult r = runAotChecked(kAsyncLambdaAssignedParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectLeakButNoMemoryError(r);         // 390 B / 20
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           async_lambda_assigned_string_param_caller_value_survives_spec_28_1) {
    JitResult r = runAot(kAsyncLambdaAssignedParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           sync_iterator_assigned_string_param_does_not_free_caller_spec_28_1) {
    JitResult r = runAotChecked(kSyncIteratorAssignedParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectLeakButNoMemoryError(r);         // 427 B / 22
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           sync_iterator_assigned_string_param_caller_value_survives_spec_28_1) {
    JitResult r = runAot(kSyncIteratorAssignedParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 64);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           async_iterator_assigned_string_param_does_not_free_caller_spec_28_1) {
    JitResult r = runAotChecked(kAsyncIteratorAssignedParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectLeakButNoMemoryError(r);         // 427 B / 22
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           async_iterator_assigned_string_param_caller_value_survives_spec_28_1) {
    JitResult r = runAot(kAsyncIteratorAssignedParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 64);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           constructor_assigned_string_param_does_not_free_caller_spec_28_1) {
    JitResult r = runAotChecked(kCtorAssignedParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectLeakButNoMemoryError(r);         // 390 B / 20
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           constructor_assigned_string_param_caller_value_survives_spec_28_1) {
    JitResult r = runAot(kCtorAssignedParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// ---------------------------------------------------------------------
// (P) / (Q) the save/restore guard — the enclosing body keeps its own state.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           enclosing_body_keeps_its_string_state_across_a_lambda_spec_27_3) {
    JitResult r = runAotChecked(kLambdaSaveRestore);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 371 B / 20 pre-fix; 934 B / 40 with
    BEZEL_EXPECT_EQ(r.exitCode, 123);      // the restore dropped
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           enclosing_body_keeps_its_string_state_across_async_lambda_spec_30_6) {
    JitResult r = runAotChecked(kAsyncLambdaSaveRestore);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 371 B / 20 pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 123);
}

// ---------------------------------------------------------------------
// (R) / (S) abandonment — Dispose while suspended at a yield.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           abandoned_sync_iterator_releases_its_locals_once_spec_29_7) {
    JitResult r = runAotChecked(kSyncIteratorAbandoned);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 5580 B / 240 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 240);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           abandoned_sync_iterator_plain_path_value_invariant_spec_29_7) {
    // The strong direction: were Dispose to release a local the resume also
    // releases, the second drop would fault under ASan; on the plain path a
    // freed-then-reused block would change the value.
    JitResult r = runAot(kSyncIteratorAbandoned);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 240);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           abandoned_async_iterator_releases_its_locals_once_spec_30_7) {
    JitResult r = runAotChecked(kAsyncIteratorAbandoned);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 5580 B / 240 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 240);
}

// ---------------------------------------------------------------------
// (T) / (U) the INLINED generic constructor body — the use-after-free direction.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           inlined_generic_ctor_borrow_local_is_not_freed_early_spec_28_1) {
    JitResult r = runAotChecked(kInlinedGenericCtorBorrowThenAssign);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // heap-use-after-free pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           inlined_generic_ctor_plain_path_value_invariant_spec_28_1) {
    JitResult r = runAot(kInlinedGenericCtorBorrowThenAssign);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           compiled_ctor_chaining_a_generic_base_is_balanced_spec_22_9) {
    JitResult r = runAotChecked(kCompiledCtorInlinesGenericBase);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 1324 B / 60 pre-fix; UAF with the
    BEZEL_EXPECT_EQ(r.exitCode, 141);      // ctor pin and no inline analysis
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           compiled_ctor_chaining_generic_base_plain_value_spec_22_9) {
    JitResult r = runAot(kCompiledCtorInlinesGenericBase);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 141);
}

// ---------------------------------------------------------------------
// (V)-(X) the remaining shapes: a returned local, a LINQ lambda, throw edges.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           lambda_returning_its_own_string_local_transfers_once_spec_27_3) {
    JitResult r = runAotChecked(kLambdaReturnsItsLocal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 1904 B / 80 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           lambda_returning_its_local_plain_path_value_spec_27_3) {
    JitResult r = runAot(kLambdaReturnsItsLocal);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           linq_block_bodied_lambda_releases_its_locals_spec_27_3) {
    JitResult r = runAotChecked(kLinqBlockLambda);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 4700 B / 200 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 250);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           lambda_returning_a_captured_local_still_retains_spec_27_5) {
    // The over-release direction of the lambda pin. Clean before AND after this
    // change; it goes RED (double free / LSan) if the candidate-set guard in
    // returnedStringLocalOwnsPlusOne is removed.
    JitResult r = runAotChecked(kLambdaReturnsCapturedLocal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 108);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           lambda_returning_a_captured_local_plain_path_value_spec_27_5) {
    JitResult r = runAot(kLambdaReturnsCapturedLocal);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 108);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           constructor_throw_edges_release_exactly_once_spec_22_9) {
    JitResult r = runAotChecked(kCtorThrowEdges);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 1904 B / 80 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_iterator_lambda_ctor_string_local_release_2817,
           constructor_throw_edges_plain_path_value_spec_22_9) {
    JitResult r = runAot(kCtorThrowEdges);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}
