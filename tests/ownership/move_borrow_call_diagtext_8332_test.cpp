// fix #8332 (spec §28.8.1 *The `move` expression*, Appendix A.7 `MoveExpr`,
// v0.355): the §28.8.1 BORROW-CALL diagnostic — the one the ownership pass
// reports for `move` applied to a call whose result is a borrow — used to
// list "or a call that is itself a fresh owned producer (a call returning
// `owned T`)" among the things `move` transfers.
//
// THE DEFECT. That clause contradicts two other things:
//
//   (1) the SPEC. §28.8.1 closes the operand set: "No other operand is
//       admitted (A.7): a fresh producer (a `new` expression, a call
//       returning `owned T`, an owned operator result) needs no marker
//       anywhere (K3.13) and takes none, so `move (new Cell(1))` is a
//       compile error at every position, an `owned` argument and an
//       owned-slot store alike; the remedy is to drop the `move`." A call
//       returning `owned T` is therefore NOT one of the things `move`
//       transfers, at ANY position.
//
//   (2) the COMPILER'S OWN SIBLING DIAGNOSTIC. Since narrow #7926, the
//       marker on a fresh producer at a binding position is rejected with
//       "the `move` marker is not written on a fresh producer (a call
//       returning `owned T`) — no live binding is invalidated, so there is
//       nothing to announce; remove `move` (§22.3, v0.307, §28.3 K3.13)".
//       The two messages are about the SAME shape and they disagreed.
//
// The fix drops the clause from the diagnostic's list. The sibling clause —
// "a named binding (a local, an `owned` parameter, or an `owned`
// field/collection slot, §28.8.1)" — is still correct (§28.8.1's own
// enumeration of the admitted operand shapes) and MUST STAY: row 2 pins it.
//
// THIS IS A TEXT FIX, NOT A BEHAVIOUR FIX. The verdict on every shape is
// unchanged: a `move` of a borrow-returning call is still refused, with the
// same diagnostic and the same remedy. Rows 1-2 pin the text; row 3 pins
// that the two `move` diagnostics cannot drift apart again.
//
// OBSERVABILITY. The pre-fix behaviour is a clean COMPILE of the program the
// text names, so the oracle is the SPECIFIC diagnostic TEXT — never
// `failedAtCompileTimeWithRealLocation` alone (which would answer true for
// any unrelated rejection) and never `!r.ran` (a runtime crash also
// synthesizes an Error diagnostic — tests/README.md).
//
// per §28.8.1, Appendix A.7 MoveExpr, v0.355

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The stable prefix of the borrow-call diagnostic.
constexpr const char* kBorrowCall = "`move` is not admitted on this call";

// The junction between the clause list and the reason: with the clause
// dropped, the list ends at "§28.8.1)" and the reason follows. BEFORE the
// fix the list continued "§28.8.1) or a call that is itself a fresh owned
// producer ...", so this needle did NOT match — it is the row's oracle.
constexpr const char* kAdmittedOperands =
    "`owned` parameter, or an `owned` field/collection slot, \xc2\xa7""28.8.1). "
    "This call returns a plain (non-`owned`) type";

// The sibling clause that must survive the fix (A.7's three admitted
// operand shapes).
constexpr const char* kNamedBinding = "transfers only a named binding";

// The clause that must NOT survive the fix.
constexpr const char* kFreshProducerClause = "fresh owned producer";

// The sibling diagnostic's own wording — "remove `move`".
constexpr const char* kRemoveMove = "remove `move`";

// True iff NO diagnostic in `r` contains `sub`. A CHECK-NOT on a vanished
// literal would otherwise be vacuous, so every row that uses this also
// asserts the POSITIVE text of the same diagnostic.
bool noDiagnosticContains(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return false;
    return true;
}

// The issue's own repro: `move` applied to a call that returns a PLAIN
// (non-`owned`) type. This is the shape the borrow-call diagnostic was
// written for, and it is the program whose message carried the stale clause.
const char* kPlainCallProg =
    "module P; "
    "class Cell "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor end destructor; "
    "end Cell; "
    "class W "
    "  public static method N(): int return 3; end N; "
    "  public static method Main(): int "
    "    var a := move W.N(); "
    "    Console.WriteLine(a.ToString()); "
    "    return 0; "
    "  end Main; "
    "end W;";

// The CONTROL shape: `move` of a call that returns a BORROW of a class —
// the #4683 repro's shape, the one the ownership pass's rule was written
// for. Its verdict must not move.
const char* kBorrowCallProg =
    "module M; "
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "  public method Self(): Widget return this; end Self; "
    "end Widget; "
    "public class Program "
    "  public static method Main(): int "
    "    using var other := new Widget(\"x\"); "
    "    using var live := move other.Self(); "
    "    Console.WriteLine(live.Ping().ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// The shape the sibling diagnostic is about: a `move` marker on a call
// returning `owned T` at a binding position (narrow #7926).
const char* kFreshProducerProg =
    "module M; "
    "class Cell "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor end destructor; "
    "end Cell; "
    "class W "
    "  public static method Mk(): owned Cell return new Cell(3); end Mk; "
    "  public static method Main(): int "
    "    var a := move W.Mk(); "
    "    Console.WriteLine(a.Id.ToString()); "
    "    return 0; "
    "  end Main; "
    "end W;";

}  // namespace

// Row 1 — the REGRESSION row: the diagnostic's clause list names exactly
// the admitted operand shapes of §28.8.1 / A.7 and no longer claims that
// `move` transfers "a call that is itself a fresh owned producer".
//
// FAILS WITHOUT THE FIX: the pre-fix message continues "§28.8.1) or a call
// that is itself a fresh owned producer (a call returning `owned T`)." — so
// the positive needle does not match and the negative needle does.
BEZEL_TEST(move_borrow_call_diagtext_8332,
           the_clause_list_names_only_the_admitted_move_operands) {
    auto r = runAot(kPlainCallProg);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    // the diagnostic still fires, on the same shape
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowCall));
    // the clause list ends at the sibling clause (A.7's three shapes)
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNamedBinding));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAdmittedOperands));
    // and the fresh-producer clause is gone
    BEZEL_EXPECT_TRUE(noDiagnosticContains(r, kFreshProducerClause));
}

// Row 2 — the CONTROL row: the shape the diagnostic was written for is
// still refused, with the SAME verdict, and the sibling clause survives.
BEZEL_TEST(move_borrow_call_diagtext_8332,
           a_borrow_returning_call_is_still_refused_with_the_same_verdict) {
    auto r = runAot(kBorrowCallProg);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowCall));
    // the sibling clause is still there (it is §28.8.1's own enumeration)
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNamedBinding));
    // and the remedy sentence is unchanged
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "Read it without `move`"));
}

// Row 3 — the two `move` diagnostics must not drift apart again: the
// fresh-producer shape is refused with "remove `move`", and the borrow-call
// diagnostic no longer claims that a fresh producer is one of the things
// `move` transfers.
BEZEL_TEST(move_borrow_call_diagtext_8332,
           the_borrow_call_and_fresh_producer_diagnostics_agree) {
    // (a) the fresh-producer shape: the sibling diagnostic, unchanged
    auto fresh = runAot(kFreshProducerProg);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(fresh));
    BEZEL_EXPECT_FALSE(fresh.ran);
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(fresh, "not written on a fresh producer"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(fresh, kRemoveMove));

    // (b) the borrow-call shape: it must not claim the opposite
    auto borrow = runAot(kPlainCallProg);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(borrow, kBorrowCall));
    BEZEL_EXPECT_TRUE(noDiagnosticContains(borrow, kFreshProducerClause));
    BEZEL_EXPECT_TRUE(noDiagnosticContains(borrow, "not written on a fresh producer"));
}
