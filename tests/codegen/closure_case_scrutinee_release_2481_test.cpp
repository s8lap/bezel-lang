// Codegen — issue #2481 (consumer 3/3: closures) / spec §19.3 ("Patterns" /
// "Pattern binding and ownership") + §28.9 (runtime-managed types; the
// closure-cell refcount discipline) + §28.3 (scope-end auto-drop) + §27.5
// (closures / capture), docs revision v0.235.
//
// #2450 (aa30f97) lifted StringEscapeWalker's unconditional "a `case`
// scrutinee is always an escape" rule for its consumers (generalized to
// plain strings and #1747 records by the first two commits of #2481). This
// file covers the THIRD, entirely separate consumer: `ClosureEscapeWalker`
// (LlvmBackend_Setup.cpp) / `collectClosureReleaseEligible` — a DIFFERENT
// C++ struct with its own escape rules (no member access, no interpolation;
// a call ARGUMENT always escapes; reference equality is the only safe binary
// op). A closure-typed local (`var f := lambda() => ...;`, owning the
// lambda's +1 heap-allocated env cell) whose only use was a `case`
// destructuring scrutinee (`case f of var g => g(), _ => ...`) never armed
// its scope-end release, leaking the cell.
//
// THE ARGUMENT (independently re-derived for ClosureEscapeWalker, NOT
// assumed to transfer from the string/record consumers, per the issue's own
// caution -- "a different walker entirely"): §19.3's "Pattern binding and
// ownership" paragraph names "function values" in the SAME sentence as
// `string` for the runtime-managed-scrutinee borrow rule: "for runtime-
// managed scrutinees (`string`, function values), the binding is a shared
// reference" -- so `case f of var g => ...` is, per the spec's own text, the
// identical shape to the string/choice case. Checked against the lowering,
// not just the wording: LlvmBackend_Exprs2.cpp's BindingPattern arm (the
// SAME code that handles string/record/choice scrutinees) stores a
// closure-typed scrutinee value into a fresh alloca via a raw `CreateStore`
// -- no retain, no closure-specific branch at all. And closures have their
// OWN complete, independent parallel of the string retain-on-escape
// machinery: `returnedClosureLocalOwnsPlusOne` is false for any
// BindingPattern decl (same `dynamic_cast<LocalDecl*>` miss as strings, and
// a BindingPattern's address is never in `closureReleaseEligible_`, which
// `collectClosureReleaseEligible`'s own candidate scan populates only from
// `ast::LocalDecl` nodes) -- so `return g;` unconditionally
// `emitClosureRetain`s; `lowerClosureFieldStore`'s producer gate has no
// bare-local-read case, so a field-store of `g` also unconditionally
// retains. The lambda-capture channel is the identical no-retain gap
// (`inLambda`, mirroring FINDING [1] for strings). A NEW,
// ClosureEscapeWalker-specific `armsBindingsSafe` (reusing the SAME shared,
// type-agnostic `collectPatternBindings` free function, but keying each
// per-binding sub-check on a FRESH `ClosureEscapeWalker` instance so the
// CORRECT escape rules apply) proves every arm's bindings non-escaping
// before admitting the scrutinee.
//
// VERIFY-FIRST: reverting just the new `w.scrutineeBorrowMode = true;`
// opt-in (collectClosureReleaseEligible's candidates loop) reproduces an
// exact 40-byte / 1-allocation LSan leak for the safe-read repro below
// (mechanically confirmed via an ASan/LSan probe before writing this test);
// clean with the fix.
//
// ADVERSARIAL GRID (risk direction is UAF -- REMOVING a conservative escape
// mark):
//   * a `var g` binding invoked (`g()`) as the CaseExpr arm result --
//     released.
//   * statement-position `case` (CaseStmt) invoking `g()` into an outer
//     local -- released.
//   * a binding CAPTURED by a lambda that escapes (returned as a closure,
//     invoked after the original `f`'s own scope-end release would have
//     fired) -- MUST stay conservative (leak, no UAF): pinned via
//     runAotCheckedLenient + a separate detect_leaks=0 correctness check.
//   * a binding STORED into another object's field that escapes its arm --
//     same conservative pin.
//   * a binding REBOUND into another local (`var c := g;`) that itself
//     escapes -- same conservative pin.
//
// Both LSan checks skip gracefully when no ASan toolchain is available.

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

// ===========================================================================
// Positive: `f`'s only use is the case scrutinee, a whole-value `var g`
// binding invoked as the arm result. Released at scope end.
// ===========================================================================
BEZEL_TEST(closure_case_scrutinee_release_2481,
           safe_invoke_arm_result_released_spec_19_3) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var f := lambda() => 42; "
        "  return (case f of var g => g(), _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Statement-position `case` (CaseStmt) -- mirrors #2450's own CaseStmt
// coverage; pins the mirrored ClosureEscapeWalker::stmt CaseStmt edit.
// ===========================================================================
BEZEL_TEST(closure_case_scrutinee_release_2481,
           case_stmt_scrutinee_released_spec_19_3) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var f := lambda() => 42; "
        "  var result := 0; "
        "  case f of "
        "    var g => do result := g(); end, "
        "    _ => do end, "
        "  end; "
        "  return result; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// UAF-risk grid: a `var g` binding CAPTURED by a lambda that escapes the
// producing method (returned as a closure, invoked after `f`'s own
// scope-end release would have fired). The new ClosureEscapeWalker
// armsBindingsSafe's inLambda rule catches this regardless of read-safety,
// so `f` stays a conservative escape. Pinned via runAotCheckedLenient + a
// separate detect_leaks=0 correctness check.
// ===========================================================================
BEZEL_TEST(closure_case_scrutinee_release_2481,
           lambda_capture_of_binding_stays_conservative_no_uaf_spec_27_5) {
    const std::string kSrc =
        "module M; class C "
        "  public static method Make(): () -> int "
        "    var f := lambda() => 42; "
        "    return (case f of var g => lambda() => g(), _ => lambda() => 0); "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := C.Make(); "
        "    return h(); "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF (generated-code ASan still active)
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// UAF-risk grid: a `var g` binding STORED into a field of an object that
// outlives the producing method's own scope. The generic AssignmentStmt
// handling classifies `b.v := g;` as an escape (no field-store
// special-case for a BindingPattern target). Pinned no-UAF + correct value.
// ===========================================================================
BEZEL_TEST(closure_case_scrutinee_release_2481,
           field_store_escape_of_binding_stays_conservative_no_uaf_spec_28_9) {
    const std::string kSrc =
        "module M; class Box public v: () -> int := lambda() => 0; end Box; "
        "class C "
        "  public static method Fill(b: Box) "
        "    var f := lambda() => 42; "
        "    case f of "
        "      var g => do b.v := g; end, "
        "      _ => do end, "
        "    end; "
        "  end Fill; "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    C.Fill(b); "
        "    return b.v(); "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF (generated-code ASan still active)
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// UAF-risk grid: a `var g` binding REBOUND into another local (`var c :=
// g;`) that itself escapes (stored into an outer-scoped `result` and
// returned). The generic LocalDecl handling classifies the rebind as an
// escape regardless of what `c` goes on to do. Pinned no-UAF + correct
// value.
// ===========================================================================
BEZEL_TEST(closure_case_scrutinee_release_2481,
           rebind_then_escape_stays_conservative_no_uaf_spec_19_3) {
    const std::string kSrc =
        "module M; class C "
        "  public static method Fill(): () -> int "
        "    var f := lambda() => 42; "
        "    var result := lambda() => 0; "
        "    case f of "
        "      var g => do var c := g; result := c; end, "
        "      _ => do end, "
        "    end; "
        "    return result; "
        "  end Fill; "
        "  public static method Main(): int "
        "    var r := C.Fill(); "
        "    return r(); "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF (generated-code ASan still active)
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
