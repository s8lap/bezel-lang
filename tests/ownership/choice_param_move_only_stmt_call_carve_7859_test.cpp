// fix #7859 — a BARE-STATEMENT call / constructor over an OWNING-CHOICE local
// moves its argument, except the §25.2 comparison carve (spec §15.3 / §25.2,
// v0.355).
//
// THE GOVERNING TEXT (this tree's line numbers).
//
//   §15.3 *Parameter passing* (spec:3048) — "a parameter declared `(x: V)` for
//   an ordinary V is by value (copy); for a move-only V, it is by value-move
//   (transfer ownership). The caller's argument expression is in moved-from
//   state after the call. (Operator operands and the comparison surface's
//   `Equals(T)` parameter are not plain parameters in this sense: they bind
//   borrows — §25.2.)" — and the owning-classified choice (with its nullable
//   wrapper `C?`) is one of the two move-only value types (§15.3, spec:3042).
//
//   §25.2 *Restrictions and resolution* (spec:5956) — "On a move-only `T`, the
//   declared `Equals(other: T): bool` parameter binds a borrow exactly as the
//   operator operands above do … and `a.Equals(b)` consumes neither value. An
//   **elective** method parameter of the declaring type (`Merge(other: C)`, say)
//   is outside the carve and remains §15.3's by-value-move."
//
//   §17.8 *The null-forgiving operator `!`* (spec:3662) — "Over a named owned
//   binding, `x!` is therefore classified by its position exactly as the bare
//   read `x` is." — so the `!` spelling and the bare spelling of ONE call are
//   the same rule.
//
//   §15.3 *Local-to-local assignment* (spec:3046) — "After the move, `x` is in
//   moved-from state and may not be read until reassigned."
//
//   §35.4 *The classification* (spec:11301) — the owning-classified choice; a
//   **value-like** choice is copied, not moved.
//
// THE DEFECT. #5436 added the owning-CHOICE arm to the two BOUND argument
// gates (`recordMovesInArgs` and `recordMovesIn`'s `new T(...)` arm) but never
// to the two BARE-STATEMENT gates inside `handleDischarge`, so an owning-choice
// local stayed live after a statement-position call took it:
// `Sink.Take(o!); var n := o;` compiled with no moved-from error.
//
// THE CARVE (the second attempt's subject). §15.3's own bullet EXCLUDES the
// comparison surface's `Equals(T)` parameter, and §25.2 says `a.Equals(b)`
// consumes neither value. The first attempt at this fix applied the by-value-
// move to that parameter too, so `a.Equals(b)` over an owning choice stopped
// compiling — a regression, and a regression of a BANKED corpus row
// (`s35_7_0005`). The two sites now consult the repo's own carve predicate,
// `moveOnlyArgIsConsumed` (#4803), so an `Equals` argument — and a `ref` /
// `out` argument — is not marked moved-from, while every other argument is.
//
// THE ORACLE. Each reject row pins the SPECIFIC moved-from diagnostic and
// uses `failedAtCompileTimeWithRealLocation`, so a crash / timeout / ASan
// report cannot be mistaken for the answer. Each accept row pins that the
// program COMPILES AND RUNS, and (where a payload is disposed) that the
// disposal count is exactly what the spec says.
//
// per §15.3 *Parameter passing*, §25.2, §17.8, §35.4, v0.355

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A payload class with a COUNTING destructor, so a double disposal (>1 per
// value) and a suppressed one (0) are both distinguishable.
const char* kRes =
    "public class Res "
    "  public static Count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.Count := Res.Count + 1; end destructor; "
    "end Res; ";

// The issue's own shape: an owning-choice local `o`, a sink that takes it by
// value, and a body the caller fills in.
std::string callProg(const std::string& body) {
    return std::string("module M; ") +
           kRes +
           "public choice OC Text(owned r: Res), Empty; end OC; "
           "public class Sink "
           "  public static method Take(e: OC?) end Take; "
           "  public static method TakePlain(e: OC) end TakePlain; "
           "end Sink; "
           "public class Program "
           "  public static method Main(): int "
           "    var o: OC? := OC.Text(new Res()); " +
           body +
           "    Console.WriteLine(\"call-ok\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The constructor twin of the shape above: a `Holder` whose constructor takes
// the owning choice by value.
std::string ctorProg(const std::string& body) {
    return std::string("module M; ") +
           kRes +
           "public choice OC Text(owned r: Res), Empty; end OC; "
           "public class Holder "
           "  private _e: OC?; "
           "  public constructor(e: OC?) _e := e; end constructor; "
           "end Holder; "
           "public class Program "
           "  public static method Main(): int "
           "    var o: OC? := OC.Text(new Res()); " +
           body +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The §25.2 carve's shape: an owning choice that DECLARES `Equals(T)`, called
// at the statement position over two owning-choice bindings. `bDecl` chooses
// the binding spelling of the ARGUMENT: a `using var` one (scope-bound — a
// wrong move out of it is a compile error, so this is the sharpest oracle
// there is) or a plain `var` one (movable — so a LATER READ of it can be
// written, and its admission is the carve's observable meaning).
std::string equalsProg(const std::string& bDecl, const std::string& body) {
    return std::string("module M; ") +
           kRes +
           "public choice Holder "
           "  Full(owned r: Res), "
           "  Empty; "
           "  public method Equals(other: Holder): bool => false; "
           "  public method GetHashCode(): int => 7; "
           "end Holder; "
           "public class Program "
           "  public static method Main(): int "
           "    using var a := Holder.Full(new Res()); " +
           bDecl +
           body +
           "    Console.WriteLine(\"eq-ok\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The elective method's shape: `Merge(other: C)` is OUTSIDE the §25.2 carve
// and remains §15.3's by-value-move.
std::string mergeProg(const std::string& body) {
    return std::string("module M; ") +
           kRes +
           "public choice Holder2 "
           "  Full(owned r: Res), "
           "  Empty; "
           "  public method Merge(other: Holder2): bool => false; "
           "end Holder2; "
           "public class Program "
           "  public static method Main(): int "
           "    using var a := Holder2.Full(new Res()); "
           "    var b := Holder2.Full(new Res()); " +
           body +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The value-like choice's shape: NOT an owning-classified choice, so §35.4
// rule 3 copies it and the argument is not moved.
std::string valueLikeProg(const std::string& body) {
    return std::string("module M; ") +
           kRes +
           "public choice OC Text(owned r: Res), Empty; end OC; "
           "public choice VL Some(n: int), None; end VL; "
           "public class Sink2 "
           "  public static method Take(e: VL?) end Take; "
           "end Sink2; "
           "public class Program "
           "  public static method Main(): int "
           "    var v: VL? := VL.Some(1); " +
           body +
           "    Console.WriteLine(\"vl-ok\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The parameter source's shape: `o` is a PARAMETER, not a local — the #7725
// parameter arm of the move-marking primitive.
std::string paramSourceProg(const std::string& body) {
    return std::string("module M; ") +
           kRes +
           "public choice OC Text(owned r: Res), Empty; end OC; "
           "public class Sink "
           "  public static method Take(e: OC?) end Take; "
           "end Sink; "
           "public class Program "
           "  public static method T(o: OC?) "
           "    " +
           body +
           "  end T; "
           "  public static method Main(): int "
           "    T(OC.Text(new Res())); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

void expectMovedFromRead(const JitResult& r) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "was already moved out"));
}

void expectRan(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace

// --- the fix: a bare-statement call / ctor over an owning choice moves it ---

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_15_3_stmt_call_over_owning_choice_local_moves_it) {
    // The issue's own shape. Pre-fix: accepted, with no diagnostic.
    expectMovedFromRead(runAot(callProg("Sink.Take(o!); var n := o; ")));
}

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_17_8_bare_read_twin_of_the_bang_read_is_rejected) {
    // §17.8: the `!` spelling and the bare spelling of ONE call are the same
    // rule. Pre-fix: accepted, with no diagnostic.
    expectMovedFromRead(runAot(callProg("Sink.Take(o); var n := o; ")));
}

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_15_3_stmt_ctor_over_owning_choice_local_is_rejected) {
    // The constructor twin of the issue's shape. Pre-fix: accepted.
    expectMovedFromRead(runAot(ctorProg("new Holder(o!); var n := o; ")));
}

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_15_3_stmt_ctor_bare_twin_is_rejected) {
    // The bare twin of the constructor twin. Pre-fix: accepted.
    expectMovedFromRead(runAot(ctorProg("new Holder(o); var n := o; ")));
}

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_15_3_stmt_call_over_parameter_source_is_rejected) {
    // A PARAMETER source (the #7725 parameter arm): the move marks the
    // parameter moved-from too. Pre-fix: accepted.
    expectMovedFromRead(
        runAot(paramSourceProg("Sink.Take(o!); var n := o; ")));
}

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_35_4_non_nullable_owning_choice_stmt_call_is_rejected) {
    // §35.4's owning-classified choice in its NON-nullable spelling. The
    // formal's type is the choice itself. Pre-fix: accepted.
    expectMovedFromRead(
        runAot(callProg("Sink.TakePlain(o); var n := o; ")));
}

// --- the carve: §25.2's `Equals(T)` parameter binds a BORROW --------------

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_25_2_equals_over_owning_choice_consumes_neither_value) {
    // §25.2: "`a.Equals(b)` consumes neither value." The first attempt at this
    // fix applied the by-value-move to this parameter too, so this row did not
    // compile. `b` is a `using var` — a scope-bound binding — so a wrong move
    // out of it is a compile error in its own right: this row is the sharpest
    // oracle the carve has.
    const JitResult r = runAot(
        equalsProg("    using var b := Holder.Full(new Res()); ",
                   "a.Equals(b); "));
    expectRan(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq-ok\n"));
}

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_25_2_equals_over_owning_choice_leaves_the_argument_live) {
    // The same call with a LATER READ of the argument: `b` is not moved-from,
    // so the read is admitted. This is the row the first attempt broke.
    const JitResult r = runAot(
        equalsProg("    var b := Holder.Full(new Res()); ",
                   "a.Equals(b); var n := b; "));
    expectRan(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq-ok\n"));
}

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_25_2_elective_merge_argument_is_a_by_value_move) {
    // The carve's right edge: §25.2 — "An **elective** method parameter of the
    // declaring type (`Merge(other: C)`, say) is outside the carve and remains
    // §15.3's by-value-move." So `a.Merge(b)` DOES consume `b`.
    expectMovedFromRead(runAot(mergeProg("a.Merge(b); var n := b; ")));
}

// --- the controls: the fix must not widen, and must not reject the call ---

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_35_4_value_like_choice_argument_is_copied_not_moved) {
    // §35.4 rule 3: a VALUE-LIKE choice is copied, not moved. The argument is
    // not move-only, so no note fires and the later read is admitted.
    const JitResult r = runAot(valueLikeProg("Sink2.Take(v!); var n := v; "));
    expectRan(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("vl-ok\n"));
}

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_15_3_stmt_call_without_a_later_read_is_admitted) {
    // The fix rejects the READ of a moved-from binding, not the call: the
    // statement-position call with no later read is admitted.
    const JitResult r = runAot(callProg("Sink.Take(o!); "));
    expectRan(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("call-ok\n"));
}

BEZEL_TEST(ownership_choice_stmt_call_carve_7859,
           spec_15_3_bound_call_over_owning_choice_local_is_rejected) {
    // The BOUND spelling already rejects this (#5436's bound path) — a
    // control: the fix must not change it.
    expectMovedFromRead(
        runAot(callProg("var d := Sink.Take(o); var n := o; ")));
}
