// Codegen — issue #2481 (consumer 1/3: plain `string` locals) / spec §19.3
// ("Patterns" / "Pattern binding and ownership") + §28.9 (runtime-managed
// string reference counting) + §28.3 (scope-end auto-drop) + §27.5 (closures
// / capture), docs revision v0.235.
//
// #2450 (aa30f97) lifted StringEscapeWalker's unconditional "a `case`
// scrutinee is always an escape" rule, but ONLY for the value-like-choice
// release passes (`valueLikeChoiceMode`, since renamed `scrutineeBorrowMode`
// -- the flag never actually inspected the target's TYPE, only that it is a
// bare-identifier scrutinee whose arm bindings are proven non-escaping). Every
// OTHER consumer of the SAME shared walker kept the prior unconditional rule,
// including the plain-`string`-local release walk (`collectStringReleaseEligible`'s
// producer/borrow candidate loops, `stringReleaseMode`) -- filed as #2481. A
// plain `var s := <string producer/borrow>; ... case s of "lit" => ..., _ =>
// ...;` never armed its scope-end release when `s`'s only use was the case
// scrutinee -- the shape ordinary string-dispatch code uses.
//
// THE ARGUMENT (re-derived for this consumer, not assumed to transfer from
// #2450): §19.3's "Pattern binding and ownership" paragraph states the SAME
// rule for ANY scrutinee, not only a choice payload: "for runtime-managed
// scrutinees (`string`, function values), the binding is a shared reference."
// A `case` over a plain `string` scrutinee can only use binding-free patterns
// (a string-literal ConstantPattern, `_`, an AltPattern of constants) or a
// whole-value BindingPattern (`var t`, `var t: string`) -- records/choice-case
// destructuring patterns cannot target a `string` scrutinee (sema rejects
// them; there is nothing to destructure). Checked directly against the
// lowering (LlvmBackend_Exprs2.cpp, the BindingPattern arm of the case-pattern
// lowering): an unconstrained/typed `var t` binding over a plain-string
// scrutinee is stored via the SAME code path as a choice payload binding --
// `entryAlloca` + a raw `builder_.CreateStore(scrut, slot)`, no retain call --
// so `returnedStringLocalOwnsPlusOne` (false for any BindingPattern decl) and
// `lowerStringFieldStore`'s retain-unless-+1-producer gate (no IdentifierExpr
// case) apply identically regardless of whether the binding's source is a
// choice payload or a bare string scrutinee. The lambda-capture no-retain gap
// (§27.5) is likewise decl-kind-agnostic. So #2450's `armsBindingsSafe` /
// `collectPatternBindings` machinery -- reused as-is, opted in via the (now
// generalized) `scrutineeBorrowMode` flag on the SAME producer/borrow walks
// that already opt into `stringReleaseMode` (#1997) -- is sound for this
// consumer too, with the SAME UAF-risk channels (lambda capture, field-store,
// rebind-then-escape) staying conservative for the SAME reason (the walker
// does not special-case those channels for a BindingPattern target).
//
// VERIFY-FIRST: reverting just the two new `w.scrutineeBorrowMode = true;`
// opt-ins (collectStringReleaseEligible's producerCandidates/borrowCandidates
// loops) reproduces an exact 38-byte / 2-allocation LSan leak for the
// safe-read-arm-result repro below (mechanically confirmed via an ASan/LSan
// probe before writing this test); clean with the fix.
//
// ADVERSARIAL GRID (risk direction is UAF -- REMOVING a conservative escape
// mark):
//   * safe-read arm result, PRODUCER-initialized scrutinee -- released.
//   * safe-read arm result, BORROW-initialized scrutinee (from a parameter)
//     -- released (the borrowCandidates loop, distinct code path from
//     producerCandidates).
//   * a whole-value `var t` binding read only in a safe position -- released.
//   * statement-position `case` (CaseStmt) -- released, mirrors the CaseExpr
//     fix via the same StringEscapeWalker::stmt edit #2450 already made.
//   * a binding CAPTURED by a lambda that escapes -- MUST stay conservative
//     (leak, no UAF): pinned via runAotCheckedLenient + a separate
//     detect_leaks=0 correctness check.
//   * a binding STORED into another object's field that escapes its arm --
//     same conservative pin (the documented missed optimization #2450 already
//     called out, unchanged here).
//   * a binding REBOUND into another local that itself escapes -- same
//     conservative pin.
//
// Both LSan checks skip gracefully when no ASan toolchain is available
// (mirrors runAotChecked's own posture).

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

// ===========================================================================
// Positive: a PRODUCER-initialized string local (`(12345).ToString()`, a
// heap-allocated, non-constant-foldable +1 producer -- NOT a literal `+`
// concat, which LLVM constant-folds into an immortal string LSan cannot
// observe) whose only use is the case scrutinee, binding-free arms
// (string-literal ConstantPattern + `_`). "12345".Length = 5.
// ===========================================================================
BEZEL_TEST(string_local_case_scrutinee_release_2481,
           producer_binding_free_arms_released_spec_19_3) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var s := (12345).ToString(); "
        "  return (case s of \"lit\" => 0, _ => s.Length); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// Positive: a whole-value `var t` BINDING (not just a binding-free arm) read
// only in a safe (member-access receiver) position as the arm result --
// armsBindingsSafe proves `t` non-escaping via its own fresh sub-walker.
// "12345".Length = 5.
// ===========================================================================
BEZEL_TEST(string_local_case_scrutinee_release_2481,
           var_binding_safe_read_released_spec_19_3) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var s := (12345).ToString(); "
        "  return (case s of \"lit\" => 0, var t => t.Length); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// Positive: a BORROW-initialized string local (`var a := p;`, p a parameter)
// -- exercises collectStringReleaseEligible's borrowCandidates loop, a
// distinct code path (and distinct StringEscapeWalker instance) from the
// producer loop above. "borrowed-val".Length = 12.
// ===========================================================================
BEZEL_TEST(string_local_case_scrutinee_release_2481,
           borrow_init_scrutinee_released_spec_28_9) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Run(p: string): int "
        "    var a := p; "
        "    return (case a of \"lit\" => 0, _ => a.Length); "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run(\"borrowed-val\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// ===========================================================================
// Statement-position `case` (CaseStmt, not CaseExpr) -- mirrors #2450's own
// CaseStmt coverage for the choice consumer; pins the SAME StringEscapeWalker
// ::stmt CaseStmt edit for this consumer's opt-in. "12345".Length = 5.
// ===========================================================================
BEZEL_TEST(string_local_case_scrutinee_release_2481,
           case_stmt_scrutinee_released_spec_19_3) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var s := (12345).ToString(); "
        "  var result := 0; "
        "  case s of "
        "    \"lit\" => do end, "
        "    _ => do result := s.Length; end, "
        "  end; "
        "  return result; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// UAF-risk grid: a `var t` binding CAPTURED by a lambda that escapes the
// producing method (stored in the returned closure, invoked after `s`'s own
// scope-end release would have fired). armsBindingsSafe's inLambda rule
// catches this regardless of read-safety, so armsBindingsSafe fails, the
// scrutinee stays a conservative escape, and `s`'s release does NOT arm.
// Pinned via runAotCheckedLenient (conservative leak is the safe, expected
// outcome) + a separate detect_leaks=0 run asserting the closure still
// returns the CORRECT value (no UAF corruption). "12345".Length = 5.
// ===========================================================================
BEZEL_TEST(string_local_case_scrutinee_release_2481,
           lambda_capture_of_binding_stays_conservative_no_uaf_spec_27_5) {
    const std::string kSrc =
        "module M; class C "
        "  public static method Make(): () -> string "
        "    var s := (12345).ToString(); "
        "    return (case s of \"lit\" => lambda() => \"none\", var t => lambda() => t); "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := C.Make(); "
        "    var r := f(); "
        "    return r.Length; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF (generated-code ASan still active)
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// UAF-risk grid: a `var t` binding STORED into a field of an object that
// outlives the producing method's own scope. armsBindingsSafe's generic
// AssignmentStmt handling classifies `b.v := t;` as an escape (it does not
// special-case field-store for a BindingPattern target -- the same documented
// missed optimization #2450 already called out for the choice consumer, not
// re-litigated here). Pinned no-UAF + correct value. "12345".Length = 5.
// ===========================================================================
BEZEL_TEST(string_local_case_scrutinee_release_2481,
           field_store_escape_of_binding_stays_conservative_no_uaf_spec_28_9) {
    const std::string kSrc =
        "module M; class Box public v: string := \"\"; end Box; "
        "class C "
        "  public static method Fill(b: Box) "
        "    var s := (12345).ToString(); "
        "    case s of "
        "      \"lit\" => do end, "
        "      var t => do b.v := t; end, "
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
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// UAF-risk grid: a `var t` binding REBOUND into another local (`var c := t;`)
// that itself escapes (stored into an outer-scoped `result` and returned).
// armsBindingsSafe's LocalDecl handling classifies the rebind as an escape
// regardless of what `c` goes on to do. Pinned no-UAF + correct value.
// "12345".Length = 5.
// ===========================================================================
BEZEL_TEST(string_local_case_scrutinee_release_2481,
           rebind_then_escape_stays_conservative_no_uaf_spec_19_3) {
    const std::string kSrc =
        "module M; class C "
        "  public static method Fill(): string "
        "    var s := (12345).ToString(); "
        "    var result := \"\"; "
        "    case s of "
        "      \"lit\" => do end, "
        "      var t => do var c := t; result := c; end, "
        "    end; "
        "    return result; "
        "  end Fill; "
        "  public static method Main(): int "
        "    var r := C.Fill(); "
        "    return r.Length; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF (generated-code ASan still active)
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
