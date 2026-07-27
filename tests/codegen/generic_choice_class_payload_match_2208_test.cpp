// Codegen regression test for issue #2208 — §35.5 (v0.226).
//
// A generic choice `Opt<T>` whose case payload's instantiated type is a
// CLASS (or any non-i32 type parameter substitution) read the WRONG
// payload type at match-arm binding — the binding's local symbol was
// typed at the BARE type-parameter resolution (`inner: T`, declaration
// = null) rather than the instantiated payload type (`inner:
// Array<int>`), so `inner.Count` failed the §35.5 / §4.4 Array<T>
// bz_array_len fast path and silently returned 0 for a valid array.
// The construction site (#2047) and the disposal / string-release
// paths already substituted through the choice's instantiation; only
// the case-arm binding path was asymmetric.
//
// The fix has two parts (a) the SEMANTIC side: thread the
// choice-instantiation substitution through `fieldTy` in the
// DestructuringPattern case-arm binding (ExprTypecheck.cpp ~11418) so
// the local symbol's declType is the INSTANTIATED payload type, and
// (b) the CODEGEN side: install a RecordSubstGuard before the payload
// ExtractValue (LlvmBackend_Exprs2.cpp ~5897) so the extracted payload
// field's LLVM type matches the specialized payload struct (without
// this the unspecialized {i32} payload slot would be GEP'd into).
//
// Both are required: (a) makes `inner.Count` take the right fast path
// at sema-level classification; (b) makes the actual extract load at
// the right LLVM type. The construction site already had (b); the
// owning-choice active-case disposal (#2047) and the value-like string
// release (also #2047) also had (b); the patternMatches arm-binding
// path was the asymmetric omission. With both fixes, a generic choice
// whose payload is a class type returns the correct value through
// every case-arm shape (naked `x`, `var x`, typed `var x: T`).
//
// Spec authority: §35.4 / §35.5 (v0.226), §14.1 / §28.1 (#2047). All
// tests run via runAot (the observable is the exit code / stdout).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& decls, const std::string& body) {
    auto r = runAot(
        std::string("module M; ") + decls +
        std::string(" class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// --- §35.5: generic choice `Opt<T>`, `T` instantiated to `Array<int>` ----

BEZEL_TEST(codegen_generic_choice_class_payload_2208,
           spec_35_5_generic_opt_array_payload_match_arm_count) {
    // §35.5: a generic choice with a class-typed payload — the arm
    // binding `inner` must receive the class reference, not a bare-type-
    // parameter typed value. `inner.Count` then takes the §4.4
    // Array<T> bz_array_len fast path and returns the actual count.
    // Before fix #2208: returned 0 (bare-T type → fast-path miss).
    auto r = run(
        "choice Opt<T> Some(value: T), None; end Opt;",
        "var arr := new Array<int>(); arr.Add(42); arr.Add(100); "
        "var o: Opt<Array<int>> := Opt<Array<int>>.Some(arr); "
        "case o of "
        "  Some(var inner) => do return inner.Count; end, "
        "  None => do return -1; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 2);
}

BEZEL_TEST(codegen_generic_choice_class_payload_2208,
           spec_35_5_generic_opt_array_payload_naked_binding) {
    // §35.5: the NAKEd-binding shape (`Some(x) => …`, no `var x`) must
    // also receive the correct payload type — same substitution rule.
    auto r = run(
        "choice Opt<T> Some(value: T), None; end Opt;",
        "var arr := new Array<int>(); arr.Add(7); arr.Add(8); arr.Add(9); "
        "var o: Opt<Array<int>> := Opt<Array<int>>.Some(arr); "
        "case o of "
        "  Some(x) => do return x.Count; end, "
        "  None => do return -1; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 3);
}

BEZEL_TEST(codegen_generic_choice_class_payload_2208,
           spec_35_5_generic_result_array_payload_match_arm_count) {
    // §35.5: the same bug shape with `Result<T, E>`, class payload in
    // the FIRST slot (Ok case). Confirms the fix is not position-
    // specific to a particular choice shape.
    auto r = run(
        "choice Result<T, E> Ok(value: T), Err(error: E); end Result;",
        "var arr := new Array<int>(); arr.Add(42); arr.Add(100); "
        "var r: Result<Array<int>, int> := Result<Array<int>, int>.Ok(arr); "
        "case r of "
        "  Ok(var inner) => do return inner.Count; end, "
        "  Err(var e) => do return e; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 2);
}

BEZEL_TEST(codegen_generic_choice_class_payload_2208,
           spec_35_5_generic_result_int_array_payload_match_arm_count) {
    // §35.5: the same bug shape with the class payload in the SECOND
    // slot (Err case). Confirms the fix is not position-specific.
    auto r = run(
        "choice Result<T, E> Ok(value: T), Err(error: E); end Result;",
        "var arr := new Array<int>(); arr.Add(11); arr.Add(22); arr.Add(33); "
        "var r: Result<int, Array<int>> := Result<int, Array<int>>.Err(arr); "
        "case r of "
        "  Ok(var v) => do return v; end, "
        "  Err(var inner) => do return inner.Count; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 3);
}

BEZEL_TEST(codegen_generic_choice_class_payload_2208,
           spec_35_5_generic_either_array_int_payload_match) {
    // §35.5: TWO class-typed payload slots in different cases — the
    // Left slot is class, the Right slot is value; both slots bind to
    // the instantiated type when their arm fires.
    auto r = run(
        "choice Either<L, R> Left(value: L), Right(value: R); end Either;",
        "var arr := new Array<int>(); arr.Add(5); arr.Add(6); "
        "var e: Either<Array<int>, int> := Either<Array<int>, int>.Left(arr); "
        "case e of "
        "  Left(var inner) => do return inner.Count; end, "
        "  Right(var n) => do return n; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 2);
}

BEZEL_TEST(codegen_generic_choice_class_payload_2208,
           spec_35_5_nested_generic_opt_opt_int_payload_match) {
    // §35.5: nested generic choices — `Opt<Opt<int>>` — must also
    // substitute the inner instantiation through the outer one. The
    // outer arm's binding is `inner: Opt<int>` (the instantiated
    // payload); the inner arm's binding is `v: int`. Both layers must
    // be substituted for the count/value to flow.
    auto r = run(
        "choice Opt<T> Some(value: T), None; end Opt;",
        "var inner: Opt<int> := Opt<int>.Some(99); "
        "var nested: Opt<Opt<int>> := Opt<Opt<int>>.Some(inner); "
        "case nested of "
        "  Some(var inner2) => do "
        "    case inner2 of "
        "      Some(var v) => do return v; end, "
        "      None => do return -1; end "
        "    end; "
        "  end, "
        "  None => do return -2; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 99);
}