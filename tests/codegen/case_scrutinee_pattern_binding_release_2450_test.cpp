// Codegen — issue #2450 / spec §19.3 ("Patterns" / "Pattern binding and
// ownership") + §35.5 ("Matching" / "The borrowing match") + §28.9
// (runtime-managed string reference counting) + §28.3 (scope-end auto-drop)
// + §27.5 (closures / capture), docs revision v0.235.
//
// #2115 / #2428 armed a scope-end release of a value-like choice local's
// active-case `string` payload(s), gated on the SAME shared
// StringEscapeWalker (LlvmBackend_Setup.cpp) that every other consumer of
// the walker uses (plain string locals, #1747 records, closures). That
// walker treated a `case` MATCH SCRUTINEE as an UNCONDITIONAL escape, so a
// value-like choice local used ONLY as a case scrutinee (the shape virtually
// all real code uses) never armed -- both of #2428's own reported repros
// stayed leaking after that fix, pinned as "no crash / no UAF" (NOT
// leak-free) in choice_string_field_call_release_2428_test.cpp.
//
// THE FIX: a new opt-in walker mode, valueLikeChoiceMode, lifts the
// scrutinee-escape rule ONLY when the scrutinee IS the tracked target local
// AND armsBindingsSafe (LlvmBackend_Setup.cpp) proves EVERY arm's payload
// pattern bindings (`var a`, a naked `a`, or a nested destructure) never
// escape their own arm -- checked by running a FRESH, maximally-conservative
// StringEscapeWalker (every other opt-in mode OFF) keyed on each binding's
// own BindingPattern node, reusing exactly the walker's existing safe-read/
// escape boundary. §35.5's borrowing match makes every payload binding a
// borrow with "no extra retain" for a runtime-managed (`string`) payload, so
// the target's own release cannot dangle UNLESS some arm lets a binding
// escape WITHOUT an independent retain -- §27.5's closure capture is the one
// channel that does not retain (relies on the captured local's own would-be
// release being suppressed, a mechanism that does not exist for an untracked
// pattern binding), so armsBindingsSafe's reuse of the walker's `inLambda`
// rule is load-bearing, not incidental (see the lambda-capture test below).
//
// VERIFY-FIRST (patch-isolated against this lane's rebase): reverting just
// the two `w.valueLikeChoiceMode = true;` opt-ins reproduces the exact
// pre-existing leaks -- 87 bytes / 4 allocations for the direct-construction
// repro (matching #2428's own reported count) and the serialization
// round-trip repro leaking identically; both PASS clean with the fix.
//
// ADVERSARIAL GRID (the risk direction here is UAF, not leak -- REMOVING a
// conservative escape mark, per the lane brief):
//   * safe-read arm result (`a.Length + b.Length`) -- released AND correct.
//   * nested `case` reading bindings only in safe positions -- released AND
//     correct.
//   * a binding CAPTURED by a lambda that escapes its arm (stored into a
//     field, invoked after the producing method returns, i.e. after the
//     target's own scope-end release has already fired) -- MUST stay
//     conservative (leak, no UAF): pinned via runAotCheckedLenient (no
//     leak-free claim) + an explicit correctness check of the closure's
//     result under a separate detect_leaks=0 run.
//   * a binding STORED into another object's field that escapes its arm --
//     same pin (conservative, no UAF), even though the underlying
//     lowerStringFieldStore retain would have made this specific shape
//     actually safe -- armsBindingsSafe does not special-case field-store
//     for a binding (only #1430-style call-arg/return channels are already
//     proven for a LocalDecl, not a BindingPattern), so this is a documented
//     missed optimization, not a soundness gap.
//   * a binding REBOUND into another local that itself escapes (`var c :=
//     a; result := c;`) -- same conservative pin.
//   * statement-position `case` (CaseStmt, not just the CaseExpr the
//     issue's own repros use) -- the SAME relaxation applies via the mirror
//     edit to StringEscapeWalker::stmt's CaseStmt arm; pinned released.
//
// Both LSan checks skip gracefully when no ASan toolchain is available
// (mirrors runAotChecked's own posture, escaping_string_refcount_test.cpp).

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

// ===========================================================================
// Positive: bindings read ONLY in safe positions (member-access receiver,
// `+` operand) as the arm result. armsBindingsSafe proves both `a` and `b`
// non-escaping (each walked with a fresh conservative sub-walker), so the
// scrutinee `m` is admitted as a borrow and released at scope end.
// "hello-world-1".Length (13) + "second-2".Length (8) = 21.
// ===========================================================================
BEZEL_TEST(case_scrutinee_pattern_binding_release_2450,
           safe_read_arm_result_released_spec_35_5) {
    auto r = runAotChecked(
        "module M; choice Msg Text(first: string, second: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Text($\"hello-world-{1}\", $\"second-{2}\"); "
        "  return (case m of Text(var a, var b) => a.Length + b.Length, _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 21);
}

// ===========================================================================
// Positive: a NESTED case whose arms read a binding only in a safe
// (member-access receiver) position -- armsBindingsSafe's per-binding
// sub-walker recurses through the (unmodified, valueLikeChoiceMode=false)
// CaseExpr handling and finds each read safe. Sel.One selects `a.Length`
// ("hello-world-1".Length == 13).
// ===========================================================================
BEZEL_TEST(case_scrutinee_pattern_binding_release_2450,
           nested_case_safe_read_released_spec_35_5) {
    auto r = runAotChecked(
        "module M; choice Msg Text(first: string, second: string), Empty; end Msg; "
        "choice Sel One, Two; end Sel; "
        "class C public static method Pick(s: Sel): int "
        "  var m := Msg.Text($\"hello-world-{1}\", $\"second-{2}\"); "
        "  return (case m of "
        "            Text(var a, var b) => (case s of One => a.Length, Two => b.Length), "
        "            _ => 0); "
        "end Pick; "
        "public static method Main(): int return C.Pick(Sel.One); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

// ===========================================================================
// UAF-risk grid: a binding CAPTURED by a lambda that is stored into a field
// of an object OUTLIVING the producing method (so the closure is invoked
// AFTER the value-like choice local's own scope-end release has already
// run). armsBindingsSafe's per-binding sub-walker sets `inLambda` on the
// lambda body (the SAME rule FINDING [1] uses for every other consumer of
// this walker), so a read of `a` inside the lambda is an escape regardless
// of read-safety -- the arm fails armsBindingsSafe, the scrutinee stays a
// conservative escape, and `m`'s release does NOT arm. Pinned via
// runAotCheckedLenient (no leak-free claim -- the conservative leak is the
// EXPECTED, safe outcome) plus a separate detect_leaks=0 run asserting the
// closure still returns the CORRECT string (no corruption from a would-be
// UAF): "hello-world-1".Length == 13.
// ===========================================================================
BEZEL_TEST(case_scrutinee_pattern_binding_release_2450,
           lambda_capture_of_binding_stays_conservative_no_uaf_spec_27_5) {
    const std::string kSrc =
        "module M; choice Msg Text(first: string, second: string), Empty; end Msg; "
        "class C "
        "  public static method Make(): () -> string "
        "    var m := Msg.Text($\"hello-world-{1}\", $\"second-{2}\"); "
        "    return (case m of Text(var a, var b) => lambda() => a, _ => lambda() => \"none\"); "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := C.Make(); "
        "    var s := f(); "
        "    return s.Length; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF (generated-code ASan still active)
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

// ===========================================================================
// UAF-risk grid: a binding STORED into a field of an object that outlives
// the producing method's own scope (so the field is read AFTER `m`'s
// scope-end release would have fired, had the relaxation wrongly admitted
// this arm). armsBindingsSafe's per-binding sub-walker classifies
// `b.v := a;` as an escape via the generic AssignmentStmt handling ("binding
// the target into ANOTHER local[/field] escapes it") -- conservative, even
// though lowerStringFieldStore's unconditional retain-unless-+1-producer
// would have made this SPECIFIC shape sound (a documented missed
// optimization, not a soundness gap: armsBindingsSafe does not special-case
// field-store for a BindingPattern target the way #1430 does for a
// LocalDecl target). Pinned no-UAF + correct value, same as above.
// ===========================================================================
BEZEL_TEST(case_scrutinee_pattern_binding_release_2450,
           field_store_escape_of_binding_stays_conservative_no_uaf_spec_28_9) {
    const std::string kSrc =
        "module M; choice Msg Text(first: string, second: string), Empty; end Msg; "
        "class Box public v: string := \"\"; end Box; "
        "class C "
        "  public static method Fill(b: Box) "
        "    var m := Msg.Text($\"hello-world-{1}\", $\"second-{2}\"); "
        "    case m of "
        "      Text(var a, var bb) => do b.v := a; end, "
        "      _ => do end, "
        "    end; "
        "  end Fill; "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    C.Fill(b); "
        "    return b.v.Length; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF (generated-code ASan still active)
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

// ===========================================================================
// UAF-risk grid: a binding REBOUND into another local (`var c := a;`) that
// itself escapes (stored into an outer-scoped `result` and returned).
// armsBindingsSafe's per-binding sub-walker classifies `var c := a;` as an
// escape via the walker's own LocalDecl handling ("binding the target into
// ANOTHER local escapes it -- the new local may itself be returned/
// stored") -- conservative, regardless of what `c` goes on to do. Pinned
// no-UAF + correct value.
// ===========================================================================
BEZEL_TEST(case_scrutinee_pattern_binding_release_2450,
           rebind_then_escape_stays_conservative_no_uaf_spec_19_3) {
    const std::string kSrc =
        "module M; choice Msg Text(first: string, second: string), Empty; end Msg; "
        "class C "
        "  public static method Fill(): string "
        "    var m := Msg.Text($\"hello-world-{1}\", $\"second-{2}\"); "
        "    var result := \"\"; "
        "    case m of "
        "      Text(var a, var bb) => do var c := a; result := c; end, "
        "      _ => do end, "
        "    end; "
        "    return result; "
        "  end Fill; "
        "  public static method Main(): int "
        "    var s := C.Fill(); "
        "    return s.Length; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF (generated-code ASan still active)
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

// ===========================================================================
// Statement-position `case` (CaseStmt, not CaseExpr) -- the issue's own
// repros both use the expression form; this pins the mirrored fix in
// StringEscapeWalker::stmt's CaseStmt arm. `Text(var a, var b) => do
// result := a.Length; end` reads `a` only in a safe (member-access
// receiver) position as a discarded statement-position arm body, so
// armsBindingsSafe admits the scrutinee and `m` is released.
// ===========================================================================
BEZEL_TEST(case_scrutinee_pattern_binding_release_2450,
           case_stmt_scrutinee_released_spec_35_5) {
    auto r = runAotChecked(
        "module M; choice Msg Text(first: string, second: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Text($\"hello-world-{1}\", $\"second-{2}\"); "
        "  var result := 0; "
        "  case m of "
        "    Text(var a, var b) => do result := a.Length; end, "
        "    _ => do end, "
        "  end; "
        "  return result; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}
