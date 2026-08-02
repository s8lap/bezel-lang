// Codegen — issue #2481 (consumer 2/3: #1747 record-call-result locals) /
// spec §19.3 ("Patterns" / "Pattern binding and ownership" / "Record
// destructuring") + §28.9 (runtime-managed string reference counting) + §9.7
// (a record has no disposer) + §28.3 (scope-end auto-drop) + §27.5 (closures
// / capture), docs revision v0.235.
//
// #2450 (aa30f97) lifted StringEscapeWalker's unconditional "a `case`
// scrutinee is always an escape" rule, generalized here (LlvmBackend_Setup.cpp,
// the renamed `scrutineeBorrowMode` flag) beyond its original value-like-choice
// scope. #2481 filed the remaining consumers as separate, separately-argued
// soundness cases. This file covers the SECOND: `collectRecordCallResultRelease
// Eligible`'s #1747 record-call-result mechanism (`recordFieldReleaseMode`) — a
// `var p := Mk();` (Mk a provably-fresh record factory per #2448's own
// callee-body-aware gate, unchanged here) whose ONLY use is a `case`
// destructuring scrutinee never armed its scope-end `recordStringFields`
// disposal, so the callee-minted +1 on each string field leaked.
//
// THE ARGUMENT (re-derived for records, NOT assumed to transfer from #2450's
// choice case, per the lane's own discipline — #1747's mechanism was already
// found unsound once, #2448, and narrowed; this commit does not touch that
// narrowing, only the escape verdict on an ALREADY-admitted candidate):
// §19.3's "Pattern binding and ownership" states the identical rule for a
// record's positional destructure as for a choice payload — "for value-type
// [members], the binding is a copy … for runtime-managed [members]
// (`string`, function values), the binding is a shared reference" — and the
// lowering confirms it (LlvmBackend_Exprs2.cpp, patternMatches' record
// DestructuringPattern arm): `CreateExtractValue(scrut, {fieldIdx})` then a
// raw `CreateStore(fieldVal, slot)`, no retain, for both the naked-binding
// and `var`-binding forms — the exact same no-retain shape #2450 verified for
// choice payloads. `collectPatternBindings` / `armsBindingsSafe` are
// UNCHANGED and untyped with respect to the DestructuringPattern's head type,
// so they already walk a record's positional destructure correctly. A
// record's own type test is a pure structural check (no user getter runs,
// unlike a class named-slot match, which is a different, unimplemented
// pattern form) — so admitting `p` as a scrutinee read is exactly as safe as
// admitting a choice scrutinee, with the SAME UAF-risk channels (lambda
// capture, field-store, rebind) staying conservative for the SAME reason
// (armsBindingsSafe does not special-case those channels for a
// BindingPattern target, record or choice).
//
// VERIFY-FIRST: reverting just the new `w.scrutineeBorrowMode = true;`
// opt-in (collectRecordCallResultReleaseEligible's candidates loop)
// reproduces an exact 40-byte / 2-allocation LSan leak for the safe-read
// repro and the CaseStmt repro below (mechanically confirmed via ASan/LSan
// probes before writing this test); clean with the fix. The rebind-then-
// escape adversarial case is LSan-clean BOTH before and after this fix
// (independently re-verified) — `armsBindingsSafe` correctly excludes `p`
// from release in BOTH configurations (the rebind `var c := name;` escapes
// unconditionally per the walker's own LocalDecl handling, unaffected by
// scrutineeBorrowMode), and the observed leak-free-ness comes from the
// PRE-EXISTING #429/#1997 local-borrow-retain chain (`name` -> `c` -> the
// reassigned `result` -> the returned/consumed `r`) correctly delivering the
// SAME +1 to a different terminal owner — not from this fix, and not a new
// double-free (no ASan hard-error in either configuration).
//
// ADVERSARIAL GRID (risk direction is UAF — REMOVING a conservative escape
// mark):
//   * safe-read (`name.Length`) as the CaseExpr arm result — released.
//   * statement-position `case` (CaseStmt) assigning `name.Length` into an
//     outer local — released.
//   * a binding CAPTURED by a lambda that escapes — MUST stay conservative
//     (leak, no UAF).
//   * a binding STORED into another object's field that escapes its arm —
//     same conservative pin.
//
// Both LSan checks skip gracefully when no ASan toolchain is available.

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

// ===========================================================================
// Positive: `p`'s only use is the case scrutinee, positional destructuring
// with naked bindings, read only in a safe (member-access receiver) arm
// result. `Mk` is a provably-fresh factory (#2448's gate, unchanged).
// "hello-1".Length = 7.
// ===========================================================================
BEZEL_TEST(record_case_scrutinee_release_2481,
           safe_read_arm_result_released_spec_19_3) {
    auto r = runAotChecked(
        "module M; record P(Name: string, X: int); "
        "class C public static method Mk(): P "
        "  return new P($\"hello-{1}\", 8); "
        "end Mk; "
        "public static method Main(): int "
        "  var p := C.Mk(); "
        "  return (case p of P(name, x) => name.Length, _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// Statement-position `case` (CaseStmt) — mirrors #2450's own CaseStmt
// coverage; pins the SAME StringEscapeWalker::stmt CaseStmt edit for this
// consumer's opt-in. "hello-1".Length = 7.
// ===========================================================================
BEZEL_TEST(record_case_scrutinee_release_2481,
           case_stmt_scrutinee_released_spec_19_3) {
    auto r = runAotChecked(
        "module M; record P(Name: string, X: int); "
        "class C public static method Mk(): P "
        "  return new P($\"hello-{1}\", 8); "
        "end Mk; "
        "public static method Main(): int "
        "  var p := C.Mk(); "
        "  var result := 0; "
        "  case p of "
        "    P(name, x) => do result := name.Length; end, "
        "    _ => do end, "
        "  end; "
        "  return result; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// UAF-risk grid: a record-field binding CAPTURED by a lambda that escapes
// the producing method (stored in the returned closure, invoked after `p`'s
// own scope-end release would have fired). armsBindingsSafe's inLambda rule
// catches this regardless of read-safety, so `p` stays a conservative
// escape and its recordStringFields disposal does NOT arm. Pinned via
// runAotCheckedLenient + a separate detect_leaks=0 correctness check.
// "hello-1".Length = 7.
// ===========================================================================
BEZEL_TEST(record_case_scrutinee_release_2481,
           lambda_capture_of_binding_stays_conservative_no_uaf_spec_27_5) {
    const std::string kSrc =
        "module M; record P(Name: string, X: int); "
        "class C "
        "  public static method Mk(): P "
        "    return new P($\"hello-{1}\", 8); "
        "  end Mk; "
        "  public static method Make(): () -> string "
        "    var p := C.Mk(); "
        "    return (case p of P(name, x) => lambda() => name, "
        "                       _ => lambda() => \"none\"); "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := C.Make(); "
        "    var r := f(); "
        "    return r.Length; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF (generated-code ASan still active)
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// UAF-risk grid: a record-field binding STORED into a field of an object
// that outlives the producing method's own scope. armsBindingsSafe's generic
// AssignmentStmt handling classifies `b.v := name;` as an escape (no
// field-store special-case for a BindingPattern target — the same documented
// missed optimization #2450 already called out, not re-litigated here).
// Pinned no-UAF + correct value. "hello-1".Length = 7.
// ===========================================================================
BEZEL_TEST(record_case_scrutinee_release_2481,
           field_store_escape_of_binding_stays_conservative_no_uaf_spec_28_9) {
    const std::string kSrc =
        "module M; record P(Name: string, X: int); "
        "class Box public v: string := \"\"; end Box; "
        "class C "
        "  public static method Mk(): P "
        "    return new P($\"hello-{1}\", 8); "
        "  end Mk; "
        "  public static method Fill(b: Box) "
        "    var p := C.Mk(); "
        "    case p of "
        "      P(name, x) => do b.v := name; end, "
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
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// UAF-risk grid / no-regression control: a record-field binding REBOUND into
// another local (`var c := name;`) that itself escapes (stored into an
// outer-scoped `result` and returned). armsBindingsSafe's LocalDecl handling
// classifies the rebind as an escape regardless of what `c` goes on to do,
// so `p` stays conservatively excluded — mechanically re-verified IDENTICAL
// (LSan-clean, no ASan hard-error, same exit code) with the fix reverted, so
// this specific leak-free-ness is PRE-EXISTING (#429/#1997's own local-
// borrow-retain chain), not introduced by this commit. Pinned no-UAF +
// correct value either way. "hello-1".Length = 7.
// ===========================================================================
BEZEL_TEST(record_case_scrutinee_release_2481,
           rebind_then_escape_no_uaf_spec_19_3) {
    const std::string kSrc =
        "module M; record P(Name: string, X: int); "
        "class C "
        "  public static method Mk(): P "
        "    return new P($\"hello-{1}\", 8); "
        "  end Mk; "
        "  public static method Fill(): string "
        "    var p := C.Mk(); "
        "    var result := \"\"; "
        "    case p of "
        "      P(name, x) => do var c := name; result := c; end, "
        "      _ => do end, "
        "    end; "
        "    return result; "
        "  end Fill; "
        "  public static method Main(): int "
        "    var r := C.Fill(); "
        "    return r.Length; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF, either configuration
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
