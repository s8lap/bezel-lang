// Codegen fix #1702 — `++`/`--` on an `owned` class-typed FIELD whose type
// overloads `operator ++`/`operator --` must apply §28.7 drop-on-overwrite:
// dispose the displaced previous value, exactly once (spec §17.6 / §28.7,
// v0.185).
//
// §17.6 (v0.185): "When `++` or `--` is applied to an `owned` field of class
// type T whose `operator ++` / `operator --` returns a new instance, the §28.7
// drop-on-overwrite rule applies: the operator result (an owned value) is
// produced first, then the displaced previous value is disposed implicitly,
// then the new value is stored — no explicit prior disposal and no warning is
// involved."
//
// The bug: the IncrementStmt operator-overload branch stored the operator result
// through the LOCAL disposal path (disposeDisplacedOwnedClassLocal), which is a
// no-op for a FIELD target — so the displaced field value was silently lost,
// leaking on every `_field++`. The plain-assignment path already routed field
// targets through lowerOwnedClassFieldStore (§28.7 load-old / null+identity-guard
// / dispose-old / store-new); the fix routes the ++/-- store the same way.
//
// SOUNDNESS: disposal must be exactly once. lowerOwnedClassFieldStore's identity
// guard (new==old → skip) and null guard (moved-from/never-assigned → skip)
// prevent a double-free; the operator borrowed the old value as its parameter, so
// disposing it after the call is safe. A non-owned target (an `int` field, an
// owned LOCAL, a borrow field) must be untouched by the field helper — the
// controls below lock that in.
//
// Oracle: Counter's ctor bumps static Counter.n (+1), its destructor decrements
// it (-1); after the owning scope ends, `Counter.n` is the disposed-exactly-once
// balance (0). runAotChecked instruments the generated code with ASan and runs
// under LSan, so a leak / double-free / use-after-free flips r.ran to false
// (-999) — the memory verdict rides the same assertion as the counter value.
//
// Spec citations: §17.6 (increment/decrement on an owned class field, v0.185),
// §28.7 (drop-on-overwrite, v0.161), §25 (operator overloading).
//
// ⚠ RETIRED by fix #3978 (§25.1/§17.6, v0.295). §25.1's not-overloadable list,
// verbatim: "The increment and decrement statements `++` and `--` — built-in
// statements over the operand types §6.6's compound form admits (§17.6), not
// operators of the expression grammar." `operator ++`/`operator --` are no
// longer admissible at ALL — the drop-on-overwrite machinery this file
// exercises (the `opMethod` branch of `LlvmBackend_Stmts.cpp`'s IncrementStmt
// handler, and `lowerOwnedClassFieldStore`'s call from it) is now dead code:
// no program can reach it, because `Counter`'s own `operator ++`/`operator --`
// declaration is rejected before any `_c++`/`_c--`/`c++` use is ever lowered.
// `field_increment_disposes_displaced_spec_28_7_v0_185`,
// `field_decrement_disposes_displaced_spec_28_7_v0_185`, and
// `owned_local_increment_still_disposes_spec_28_7` all shared `kCounterHdr`
// (which declares BOTH `operator ++` and `operator --`), so all three fail
// identically at the SAME declaration-site diagnostic regardless of which
// operator or which lvalue category (field vs. local) they exercised —
// consolidated into ONE reject test below rather than kept as three
// duplicate pins of the same rejection. The soundness CONTROL
// (`non_owned_int_field_increment_unchanged_spec_17_6`, no `Counter`
// involved at all) is unaffected and unchanged.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Compile with generated-code AddressSanitizer, run under LeakSanitizer, return
// the exit code. A leak / double-free / use-after-free FAILS the run (ran=false
// -> -999), so the sanitizer verdict rides the same assertion as the value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
}

bool diagContains1702(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Counter: ctor +1 / dtor -1; operator ++/-- return a fresh owned Counter.
const std::string kCounterHdr =
    "module M; "
    "class Counter "
    "  public static n: int := 0; "
    "  public Value: int; "
    "  public constructor(v: int) n := n + 1; Value := v; end constructor; "
    "  public static method operator ++(c: Counter): owned Counter => "
    "    new Counter(c.Value + 1); "
    "  public static method operator --(c: Counter): owned Counter => "
    "    new Counter(c.Value - 1); "
    "  destructor n := n - 1; end destructor; "
    "end Counter; ";

}  // namespace


// ⚠ CONSOLIDATED by fix #3978 (§25.1/§17.6, v0.295) from three separate
// pre-fix tests (`field_increment_disposes_displaced_spec_28_7_v0_185`,
// `field_decrement_disposes_displaced_spec_28_7_v0_185`, and
// `owned_local_increment_still_disposes_spec_28_7`, kept below in this
// comment for history): each pinned that `_c++`/`_c--`/plain-local `c++`
// through `Counter`'s `operator ++`/`operator --` disposed the displaced
// value exactly once. §25.1/§17.6 retire the overload mechanism outright —
// `Counter`'s declaration is rejected on its own, so every one of those
// three programs now fails identically at the SAME declaration-site
// diagnostic (the field/local/op distinction they exercised is moot once
// the declaration itself cannot compile).
BEZEL_TEST(owned_field_increment_1702,
           counter_operator_declaration_rejected_spec_25_1_issue_3978) {
    auto r = runAot(kCounterHdr +
        "class Box "
        "  public owned _c: Counter; "
        "  public constructor() _c := new Counter(0); end constructor; "
        "  public method Bump() _c++; end Bump; "
        "  public method Dec() _c--; end Dec; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do var c := new Counter(0); c++; c.Dispose(); "
        "    using var b := new Box(); b.Bump(); b.Dec(); "
        "  end; "
        "  return Counter.n; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains1702(r, "operator `++` may not be overloaded (§25.1)"));
    BEZEL_EXPECT_TRUE(diagContains1702(r, "operator `--` may not be overloaded (§25.1)"));
}

// CONTROL (soundness): a NON-owned `int` field `_count++` must be untouched by
// the owned-field-store helper — plain numeric increment, no disposal machinery,
// no leak. Guards against the field helper mis-firing on a non-owned target.
BEZEL_TEST(owned_field_increment_1702,
           non_owned_int_field_increment_unchanged_spec_17_6) {
    expectDisposedAndLeakFree(
        "module M; "
        "class Box "
        "  public _count: int := 0; "
        "  public constructor() end constructor; "
        "  public method BumpCount() _count++; end BumpCount; "
        "  public method GetCount(): int => _count; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  b.BumpCount(); b.BumpCount(); b.BumpCount(); "
        "  return b.GetCount(); "                         // 3
        "end Main; end C;", 3);
}

// ⚠ REMOVED by fix #3978 (§25.1/§17.6, v0.295): the pre-fix
// `owned_local_increment_still_disposes_spec_28_7` control ("an owned class
// LOCAL `c++` must still dispose the displaced value through
// disposeDisplacedOwnedClassLocal ... Leak-clean") is folded into
// `counter_operator_declaration_rejected_spec_25_1_issue_3978` above — it
// used the same `kCounterHdr` `Counter` class and so fails at the identical
// declaration-site diagnostic; kept as one program there rather than a
// fourth duplicate rejection pin.
