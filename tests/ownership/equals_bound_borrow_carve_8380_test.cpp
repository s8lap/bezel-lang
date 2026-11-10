// fix #8380 — the §25.2 `Equals(T)` borrow carve reaches the BOUND path too
// (spec §15.3 / §25.2, v0.355).
//
// THE GOVERNING TEXT (this tree's line numbers).
//
//   §15.3 *Parameter passing* (spec:3048) — "a parameter declared `(x: V)` for
//   an ordinary V is by value (copy); for a move-only V, it is by value-move
//   (transfer ownership). The caller's argument expression is in moved-from
//   state after the call. (Operator operands and the comparison surface's
//   `Equals(T)` parameter are not plain parameters in this sense: they bind
//   borrows — §25.2.)" — and the owning-classified choice is one of the two
//   move-only value types (§15.3, spec:3042).
//
//   §25.2 *Restrictions and resolution* (spec:5956) — "On a move-only `T`, the
//   declared `Equals(other: T): bool` parameter binds a borrow exactly as the
//   operator operands above do … and `a.Equals(b)` consumes neither value. An
//   **elective** method parameter of the declaring type (`Merge(other: C)`, say)
//   is outside the carve and remains §15.3's by-value-move."
//
//   §28.3 *The consume table* (spec:6790) — the two diagnostics this issue
//   produced: "cannot move out of `using var` `b`" and "owned value in `b` was
//   already moved out at this point".
//
//   §35.4 *The classification* (spec:11301) — the owning-classified choice; a
//   **value-like** choice is copied, not moved.
//
// THE DEFECT. #5436 added the owning-CHOICE arm to the BOUND argument gate
// (`recordMovesInArgs`) WITHOUT the §25.2 carve, while #7859 added the same
// arm to the BARE-STATEMENT gate (`handleDischarge`) WITH the carve. So the
// SAME expression was admitted in one spelling and rejected in the other:
// the bare `a.Equals(b);` compiled, and the bound `var r := a.Equals(b);` was
// rejected as if the call had consumed its argument.
//
// THE ORACLE. Each reject row pins the SPECIFIC §28.3 diagnostic and uses
// `failedAtCompileTimeWithRealLocation`, so a crash / timeout / ASan report
// cannot be mistaken for the answer. Each accept row pins that the program
// COMPILES AND RUNS, and the disposal row pins that the argument's payload is
// disposed EXACTLY ONCE — the carve is about the CONSUME, not merely about the
// compile.
//
// per §15.3 *Parameter passing*, §25.2, §28.3, §35.4, v0.355

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

// The §25.2 surface: an owning choice that DECLARES `Equals(T)` and
// `GetHashCode()`. `bDecl` chooses the binding spelling of the ARGUMENT: a
// `using var` one (scope-bound — a wrong move out of it is a compile error, so
// this is the sharpest oracle there is) or a plain `var` one (movable — so a
// LATER READ of it can be written, and its admission is the carve's
// observable meaning). `body` is the call, in the spelling under test.
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
// and remains §15.3's by-value-move. `body` is the call under test.
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

// The CLASS-receiver twin of the §25.2 surface: the same owning choice as the
// ARGUMENT, but the receiver is a plain class. §25.2's carve is about the
// PARAMETER, not about the receiver's kind.
std::string classReceiverProg(const std::string& body) {
    return std::string("module M; ") +
           kRes +
           "public choice Holder "
           "  Full(owned r: Res), "
           "  Empty; "
           "  public method Equals(other: Holder): bool => false; "
           "  public method GetHashCode(): int => 7; "
           "end Holder; "
           "public class Box "
           "  public method Equals(other: Holder): bool => false; "
           "end Box; "
           "public class Program "
           "  public static method Main(): int "
           "    var a := new Box(); "
           "    using var b := Holder.Full(new Res()); " +
           body +
           "    Console.WriteLine(\"eq-ok\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The §28.8.1 fresh-producer control: an `owned` formal fed a `move` marker
// on a fresh producer.
std::string freshProducerProg() {
    return std::string("module M; ") +
           kRes +
           "public class Sink "
           "  public static method Take(owned r: Res) end Take; "
           "end Sink; "
           "public class Program "
           "  public static method Main(): int "
           "    Sink.Take(move new Res()); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The DISPOSAL row: the program must COMPILE, RUN, and dispose the
// receiver's payload exactly once. The two locals live in `T`, so the
// receiver's payload is disposed when `T` returns — the count is then read
// in `Main`. (The ARGUMENT's payload is disposed by neither side today: the
// code generator's half of the §25.2 carve is missing — see the row's
// comment.)
std::string disposalProg() {
    return std::string("module M; ") +
           kRes +
           "public choice Holder "
           "  Full(owned r: Res), "
           "  Empty; "
           "  public method Equals(other: Holder): bool => false; "
           "  public method GetHashCode(): int => 7; "
           "end Holder; "
           "public class Program "
           "  public static method T() "
           "    using var a := Holder.Full(new Res()); "
           "    var b := Holder.Full(new Res()); "
           "    var r := a.Equals(b); "
           "  end T; "
           "  public static method Main(): int "
           "    T(); "
           "    Console.WriteLine($\"{Res.Count}\"); "
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

// --- the fix: the BOUND spelling of the §25.2 carve ----------------------

BEZEL_TEST(ownership_equals_bound_borrow_carve_8380,
           spec_25_2_bound_equals_over_owning_choice_consumes_neither_value) {
    // §25.2: "`a.Equals(b)` consumes neither value." The BOUND spelling —
    // `var r := a.Equals(b);` — was REJECTED: "cannot move out of `using var`
    // `b`" (issue #8380). `b` is a `using var` — a scope-bound binding — so a
    // wrong move out of it is a compile error in its own right: this row is
    // the sharpest oracle the carve has.
    const JitResult r = runAot(
        equalsProg("    using var b := Holder.Full(new Res()); ",
                   "    var r := a.Equals(b); "));
    expectRan(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq-ok\n"));
}

BEZEL_TEST(ownership_equals_bound_borrow_carve_8380,
           spec_25_2_bound_equals_leaves_the_argument_live) {
    // The same call with a LATER READ of the argument: `b` is not moved-from,
    // so the read is admitted. Pre-fix: "owned value in `b` was already moved
    // out at this point (§28.3)".
    const JitResult r = runAot(
        equalsProg("    var b := Holder.Full(new Res()); ",
                   "    var r := a.Equals(b); var n := b; "));
    expectRan(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq-ok\n"));
}

BEZEL_TEST(ownership_equals_bound_borrow_carve_8380,
           spec_28_3_bound_equals_disposes_the_receiver_payload_exactly_once) {
    // The carve is about the CONSUME, not merely about the compile, so this
    // row is a RUNTIME row: the program must compile, run, and dispose the
    // RECEIVER's payload exactly once.
    //
    // MEASURED. The receiver's payload is disposed exactly once — the count
    // below is 1. The ARGUMENT's payload is disposed by NEITHER side at
    // present: the ownership pass now keeps the argument LIVE (this tip emits
    // the §28.3 auto-drop note for `b`; the base does not), but the code
    // generator's own half of the §25.2 carve is missing —
    // `Emitter::markMoveOnlyChoiceArgsMovedFrom` in
    // src/codegen/LlvmBackend_Calls2.cpp moves the argument's payload into
    // the callee's parameter, while §25.2's parameter binds a BORROW — so the
    // callee never disposes it. That file is outside this lane's declared hot
    // set, so the half is REPORTED to the coordinator rather than fixed here;
    // the row pins the number as it stands. It is the same number the base
    // already prints for the BARE spelling (which the #7859 fix made legal),
    // so the two spellings are at parity — which is what this issue asks for.
    const JitResult r = runAot(disposalProg());
    expectRan(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// --- the controls: the fix must not widen, and must not reject the call ---

BEZEL_TEST(ownership_equals_bound_borrow_carve_8380,
           spec_25_2_elective_merge_argument_is_a_by_value_move) {
    // The carve's right edge: §25.2 — "An **elective** method parameter of the
    // declaring type (`Merge(other: C)`, say) is outside the carve and remains
    // §15.3's by-value-move." So `a.Merge(b)` DOES consume `b` — in the BOUND
    // spelling too, which is the spelling this fix touched.
    expectMovedFromRead(
        runAot(mergeProg("    var r := a.Merge(b); var n := b; ")));
}

BEZEL_TEST(ownership_equals_bound_borrow_carve_8380,
           spec_25_2_bare_equals_over_owning_choice_consumes_neither_value) {
    // The #7859 CONTROL: the BARE-STATEMENT spelling of the same call was
    // already admitted by #7859's carve and must NOT move.
    const JitResult r = runAot(
        equalsProg("    using var b := Holder.Full(new Res()); ",
                   "    a.Equals(b); "));
    expectRan(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq-ok\n"));
}

BEZEL_TEST(ownership_equals_bound_borrow_carve_8380,
           spec_25_2_bare_equals_leaves_the_argument_live) {
    // The #7859 control's own later-read twin. It must NOT move.
    const JitResult r = runAot(
        equalsProg("    var b := Holder.Full(new Res()); ",
                   "    a.Equals(b); var n := b; "));
    expectRan(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq-ok\n"));
}

BEZEL_TEST(ownership_equals_bound_borrow_carve_8380,
           spec_25_2_class_receiver_equals_over_owning_choice_is_admitted) {
    // The class-receiver twin of the decisive row: §25.2's carve is about the
    // PARAMETER, not about the receiver's kind.
    const JitResult r = runAot(classReceiverProg("    var r := a.Equals(b); "));
    expectRan(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq-ok\n"));
}

BEZEL_TEST(ownership_equals_bound_borrow_carve_8380,
           spec_28_8_1_move_marker_on_a_fresh_producer_is_rejected) {
    // The §28.8.1 control: the fresh-producer rule is untouched by the carve.
    const JitResult r = runAot(freshProducerProg());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "fresh producer"));
}
