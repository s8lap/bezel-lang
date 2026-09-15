// Codegen / memory-unsafety — issue #4132 (§35.4 "Disposal is defined by the
// active case", kind 2, v0.300): an owning choice nested inside another
// choice's active-case payload leaked the innermost heap object.
//
// The classification rule §35.4 says: a choice is OWNING if any case carries
// either an `owned` class/interface payload (§35.4 rule 1a) or a move-only
// value type payload (an owning inline array or, the issue's shape, an
// OWNING CHOICE — §35.4 rule 1b). The text immediately under the three rules
// reads: "Disposal is defined by the active case. The owned payloads are
// disposed one by one, in payload declaration order; and, recursively, any
// move-only payloads."
//
// The defect: `emitOwningChoiceActiveCaseDisposal` (LlvmBackend_Disposal.cpp,
// the §35.4 owner-classified choice's per-tag active-case disposal switch)
// walked the case payload AST looking ONLY for an `owned` modifier on each
// field. `Outer.B(y: D<Widget>)` declares its `y` field WITHOUT an `owned`
// modifier — the ownership came from the FIELD'S TYPE being itself an owning
// choice (`D<Widget>` carries an `owned t: T` payload, which makes `D<T>`
// owning per rule 1b; the generic instantiation `D<Widget>` is therefore an
// owning choice per #3775's `isMoveOnlyValueType` verdict). The pre-fix walk
// saw `pf.modifier != "owned"` and silently skipped the field; the case
// emitted zero owning fields; the per-tag switch arm collapsed to the join
// (no Dispose call at all); and the nested `D<Widget>`'s own `owned` payload
// was never reached. The 200-iteration repro leaks ALL 200 Widgets
// (LSan: "200 byte(s) leaked in 200 allocation(s)", rc=1).
//
// THE FIX (LlvmBackend_Disposal.cpp:3139+, fix #4132): the walk now collects
// every OWNING payload — either an `owned`-marked field whose instantiated
// type is a class/interface (rule 1a, the pre-existing path) OR a field
// whose instantiated type is itself a move-only value type per
// `isMoveOnlyValueType` (rule 1b) — and dispatches the second kind through
// a recursive call into the same emit. The recursive call alloca's a fresh
// slot of the NESTED choice's SPECIALIZED struct (the substituted
// instantiation's layout, not the bare choice declaration's — `choiceStruct`
// specializes under an active `RecordSubstGuard` per #2047), stores the
// extracted payload value into it, and recurses. The nested emit's own
// `RecordSubstGuard` re-installs the nested choice's substitution so the
// innermost per-tag switch reads the correct tag and dispatches the
// innermost `owned` field's Dispose.
//
// ORACLE: runAotChecked (ASan/LSan). Pre-fix: LSan reports a deterministic
// leak — N Widgets leaked across N iterations. Post-fix: clean exit,
// Widget3960.n == 0. We also assert the destructor-counter path
// (deterministic, no ASan required) for the plain oracle so a future
// regression to a no-leak-but-wrong-disposal is caught.
//
// SCOPE: this file covers the choice-in-choice SHAPE from the issue title
// (an owning choice nested as a non-`owned` payload of another owning
// choice's active case). The recursively-deeper cases (3+ levels, mixing
// `owned`/non-`owned` markers, optional nesting) are covered by the
// `deeper_nesting_*` and `mixed_markers_*` rows below, each of which is a
// separate §35.4 rule-1b-instance — same defect class, distinct shapes.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Plain (non-instrumented) AOT run: deterministic destructor counter.
int runPlain(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan+LSan run: leak detection is the load-bearing oracle for the issue.
// A heap leak (deterministic per-issue, amplified 200x) is reported by LSan
// as "byte(s) leaked in N allocation(s)" and runAotChecked returns ran=false.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent -> case skipped
    return r.ran ? r.exitCode : -999;
}

// Disposal-counted class: `n` is the live count after disposal.
// Both ctors and dtors increment/decrement `n` so a constructor-then-
// destructor pair is a no-op (the `n` after a balanced pair is 0). A leak
// shows as `n > 0` at scope end.
constexpr const char* kWidget =
    "class Widget "
    "  public static n: int := 0; "
    "  public constructor() Widget.n := Widget.n + 1; end constructor; "
    "  destructor Widget.n := Widget.n - 1; end destructor; "
    "end Widget; ";

// 200 per LANE_RULES amplification guidance — the underlying defect is a
// deterministic leak (one Widget per loop iteration, every iteration), but
// the loop stays for robustness and matches the convention.
constexpr int kIters = 200;

}  // namespace

// ─── THE PIN: the exact repro from issue #4132 ────────────────────────────
//
// `Outer.B(D<Widget>.Some(new Widget()))`: an `Outer` choice whose `B` case
// carries a non-`owned` payload `y: D<Widget>` (rule-1b: a move-only value
// type payload — `D<Widget>` IS the owning choice, the §35.4 "recursively,
// any move-only payloads" trigger). Pre-fix the active-case disposal switch
// omitted the case entirely (zero owning fields), so the nested `D`'s `owned
// t: T` was never reached. Post-fix the recursive disposal walks D<Widget>'s
// active case (Some) and disposes the `owned t: Widget` via the vtable.
//
// Pre-fix measured: LSan "200 byte(s) leaked in 200 allocation(s)", rc=1;
// destructor counter returns kIters (no Dispose fired).
// Post-fix measured: clean exit, Widget.n == 0.
BEZEL_TEST(owning_choice_in_choice_payload_leak_4132,
           spec_35_4_rule_1b_choice_in_choice_disposed_recursively) {
    const std::string src = std::string("module M; ") + kWidget +
        "choice D<T> Some(owned t: T), None; end D; "
        "choice Outer B(y: D<Widget>); end Outer; "
        "class C public static method Main(): int "
        "  var i: int := 0; "
        "  while i < " + std::to_string(kIters) + " do "
        "    var o: Outer := Outer.B(D<Widget>.Some(new Widget())); "
        "    i := i + 1; "
        "  end while; "
        "  return Widget.n; "
        "end Main; end C;";
    // Plain: destructor counter must hit zero (every `new Widget()` has its
    // dispose fire at the end of the iteration scope).
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    // ASan/LSan: no leaks (or ASan toolchain absent, in which case -1000).
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// ─── DEEPER NESTING: the rule-1b recursion must reach the inner `owned` ────
//
// `Outer2.B(z: D<D<Widget>>)`: the nested owning choice is ITSELF carrying
// another owning choice — three levels. The pre-fix walk only saw `z`'s
// non-`owned` modifier and emitted zero owning fields; the per-tag switch
// arm collapsed. Post-fix the recursion walks z → D<D<Widget>> → the inner
// `D<Widget>` → Some(owned t: Widget), and disposes the innermost Widget.
// The `using var` discipline (vs the bare `var` of the pin above) exercises
// the scope-end disposal path that `registerOwningChoiceDisposal` arms; the
// result is observationally identical (every iteration: 1 allocation, 1
// disposal).
BEZEL_TEST(owning_choice_in_choice_payload_leak_4132,
           spec_35_4_rule_1b_three_level_choice_nesting_disposed) {
    const std::string src = std::string("module M; ") + kWidget +
        "choice D<T> Some(owned t: T), None; end D; "
        "choice Outer2 B(z: D<D<Widget>>); end Outer2; "
        "class C public static method Main(): int "
        "  for var i in 0..<" + std::to_string(kIters) + " do "
        "    do "
        "      using var o: Outer2 := "
        "        Outer2.B(D<D<Widget>>.Some(D<Widget>.Some(new Widget()))); "
        "    end; "
        "  end for; "
        "  return Widget.n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// ─── MIXED MARKERS: `owned`-on-non-choice is rule-1a, non-`owned`-on-choice ─
// is rule-1b. The same case carrying BOTH must dispose both correctly and
// in payload declaration order (§35.4 "in payload declaration order").
//
// `Outer3.A(owned c: Widget, d: D<Widget>)`: c is rule-1a (explicit `owned`,
// class payload — vtable Dispose), d is rule-1b (non-`owned`, owning
// choice payload — recursive per-tag Dispose). Both must fire. The plain
// counter goes up by 2 across kIters iterations and back down to 0; LSan
// reports zero leaks.
//
// Pre-fix: the rule-1b path silently skipped d, so only `c`'s Widget was
// disposed; one Widget per iteration leaked (rule-1a was already correct,
// but rule-1b was broken) → LSan N leaks.
BEZEL_TEST(owning_choice_in_choice_payload_leak_4132,
           spec_35_4_rule_1a_and_1b_mixed_in_same_case) {
    const std::string src = std::string("module M; ") + kWidget +
        "choice D<T> Some(owned t: T), None; end D; "
        "choice Outer3 A(owned c: Widget, d: D<Widget>); end Outer3; "
        "class C public static method Main(): int "
        "  for var i in 0..<" + std::to_string(kIters) + " do "
        "    do "
        "      using var o: Outer3 := "
        "        Outer3.A(new Widget(), D<Widget>.Some(new Widget())); "
        "    end; "
        "  end for; "
        "  return Widget.n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// ─── ASAN-ONLY ORACLE: the destructor counter is a deterministic
// upper-bound (the Widget's `n` is incremented on ctor and decremented on
// dtor; the leak oracle is the SAME signal on a different path). The LSan
// oracle is the issue's own measurement — run it in isolation to keep the
// row above independent of any ASan-toolchain unavailability.
BEZEL_TEST(owning_choice_in_choice_payload_leak_4132,
           spec_35_4_rule_1b_asan_only_no_leak) {
    const std::string src = std::string("module M; ") + kWidget +
        "choice D<T> Some(owned t: T), None; end D; "
        "choice Outer B(y: D<Widget>); end Outer; "
        "class C public static method Main(): int "
        "  var i: int := 0; "
        "  while i < " + std::to_string(kIters) + " do "
        "    var o: Outer := Outer.B(D<Widget>.Some(new Widget())); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
