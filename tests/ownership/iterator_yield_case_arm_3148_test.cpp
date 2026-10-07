// Ownership — fix #3148: `bodyContainsYield` (src/ownership/Ownership.cpp) was a
// hand-maintained six-arm statement list that never gained `ast::CaseStmt`.
//
// Spec (revision v0.252):
//   §29.3 "Iterators" — "A method whose body uses `yield` is an iterator"; the
//     construct is a STATEMENT (grammar A.6), so any statement position in the
//     body counts, `case`/`when` arms included.
//   §29.8 "Owned parameters of an iterator" — an `owned` parameter an iterator
//     body never moves out is disposed by the FRAME at teardown, not at the end
//     of the factory call that produced the enumerable.
//   §28.3 — the ordinary (non-iterator) end-of-method auto-drop, which is the
//     WRONG regime for an iterator: the factory returns immediately, so dropping
//     there either frees the parameter before the body ever runs or, as here,
//     drops nothing at all and the value leaks.
//
// THE DEFECT. `bodyContainsYield` recursed into exactly six kinds — YieldStmt,
// ForStmt, WhileStmt, IfStmt, DoEndBlock and the TryStmt body. `ast::CaseStmt`
// was absent, and `when` statements parse into that same node
// (Parser_Stmts.cpp::parseWhenStmt, null scrutinee), so BOTH spellings were
// invisible. `sawYield_` (Ownership.cpp:136) therefore stayed false for an
// iterator whose only `yield` sits in a `case`/`when` arm, and the §29.8
// owned-parameter frame auto-dispose at Ownership.cpp:963 never fired.
//
// The function's own header comment claimed it "Mirrors ExprTC::
// stmtsContainYield" — and that sibling (src/sema/ExprTypecheck.cpp:507-514)
// HAS the `CaseStmt` arm, added by #1107/#16; so does codegen's
// `Emitter::containsYield` (LlvmBackend_Iterators.cpp:433-435). The mirror was
// simply never re-synchronised, which left codegen emitting a state machine for
// a body the ownership pass was analysing as an ordinary method.
//
// Measured on 701cb2d, `bezelc --sanitize=address`:
//   case-arm form : "implicitly disposed at the end of the method (§28.3
//                    auto-drop)" + LeakSanitizer: 16 byte(s) leaked  -> exit 1
//   if-branch form: "implicitly disposed by the frame at teardown
//                    (§28.3/§29.8/§30.1 auto-drop)" + clean           -> exit 6
//
// THE FIX is structural rather than one arm: `bodyContainsYield` is now a walk
// whose arms are the complete `: Stmt` hierarchy of src/ast/Node.hpp in that
// header's order, with a per-kind statement of why each of the ten
// expression-only kinds cannot carry a `yield`, so the list cannot drift again.
//
// fail-without/pass-with: the two `case`/`when` cases below report
// `r.ran == false` (LeakSanitizer fires) without the fix and the expected exit
// code with it. The remaining cases are controls that pass either way — in
// particular `non_iterator_case_owned_param_still_plain_auto_drop`, which pins
// that the new arm did not turn an ordinary method containing a `case` into an
// iterator.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A disposable-by-ownership payload plus a consumer that drains the iterator.
// `Gen` takes `owned r` and never moves it out, so §29.8 makes the FRAME
// responsible for disposing it at teardown. The body reads `r.v` AFTER its
// first suspension, which is only well-defined if the parameter really is
// frame-owned rather than dropped at the factory call.
std::string wrap(const std::string& genBody) {
    return std::string(
        "module M; "
        "class Res "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Res; "
        "class C "
        "  public static method Gen(n: int, owned r: Res): IEnumerable<int> ") +
        genBody +
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var x in Gen(1, new Res(5)) do total := total + x; end; "
        "    return total; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ── FAILS WITHOUT THE FIX ───────────────────────────────────────────────────

// §29.3 / §29.8: the only `yield` is inside a `case` STATEMENT arm. Without the
// `CaseStmt` arm the method was not an iterator to the ownership pass, the
// §29.8 frame auto-dispose never fired, and `r` leaked. Value 1 + 5 = 6.
// // per §29.3 / §29.8, v0.252
BEZEL_TEST(iterator_yield_case_arm_3148, case_arm_only_yield_owned_param_frame_disposed) {
    auto r = runAotChecked(wrap(
        "    case n of "
        "      1 => do yield return 1; yield return r.v; end, "
        "      _ => do yield return 0; end, "
        "    end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// §29.3 / §29.8: the `case` STATEMENT arm must be recognised as an iterator
// body even when the yield is in the arm. Value 1 + 5 = 6.
// (The `when` spelling was retired per §19 / §22.4, v0.289; this row now uses
// the equivalent `case n of` form.) // per §29.3 / §29.8, v0.252
BEZEL_TEST(iterator_yield_case_arm_3148, when_arm_only_yield_owned_param_frame_disposed) {
    auto r = runAotChecked(wrap(
        "    case n of "
        "      1 => do yield return 1; yield return r.v; end, "
        "      _ => do yield return 0; end, "
        "    end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// §29.3: a `case` arm nested inside an `if` branch — the recursion must reach a
// `case` at any depth, not just at the top of the body. Value 1 + 5 = 6.
// // per §29.3 / §29.8, v0.252
BEZEL_TEST(iterator_yield_case_arm_3148, nested_case_in_if_yield_owned_param_frame_disposed) {
    auto r = runAotChecked(wrap(
        "    if n > 0 then "
        "      case n of "
        "        1 => do yield return 1; yield return r.v; end, "
        "        _ => do yield return 0; end, "
        "      end; "
        "    end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// ── CONTROLS (pass with and without the fix) ────────────────────────────────

// The shape the six-arm list DID cover, kept as the differential control: the
// same program with the `case` spelled as an `if`. It was already correct, and
// its exit code is what the three cases above must now also produce.
// // per §29.3 / §29.8, v0.252
BEZEL_TEST(iterator_yield_case_arm_3148, if_branch_yield_owned_param_frame_disposed_control) {
    auto r = runAotChecked(wrap(
        "    if n = 1 then yield return 1; yield return r.v; "
        "    else yield return 0; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// ★ THE FALSE-FLIP CONTROL. A `case` statement in a body with NO `yield`
// anywhere must stay an ordinary method: §28.3 end-of-method auto-drop, not the
// §29.8 frame regime. If the new arm were over-broad (e.g. treating any `case`
// as evidence of a yield) this ordinary method would be routed to the iterator
// machinery and its `owned` parameter would be disposed by a frame that does
// not exist. Value 5. // per §28.3 / §29.3, v0.252
BEZEL_TEST(iterator_yield_case_arm_3148, non_iterator_case_owned_param_still_plain_auto_drop) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Res; "
        "class C "
        "  public static method Take(n: int, owned r: Res): int "
        "    var got := 0; "
        "    case n of "
        "      1 => do got := r.v; end, "
        "      _ => do got := 0; end, "
        "    end; "
        "    return got; "
        "  end Take; "
        "  public static method Main(): int return Take(1, new Res(5)); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
