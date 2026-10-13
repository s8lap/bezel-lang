// Sema tests — #1245: ReadOnly spans are NOT assignable to writable counterparts.
//
// Spec authority: §4.14 (owning Span<T>/ReadOnlySpan<T> over OwningBuffer<T>),
//                 §4.15 (borrowing ArraySpan<T>/ReadOnlyArraySpan<T> over Array<T>),
//                 §5.1 (implicit-conversion table — span types are nominal, not
//                       structural, so cross-kind coercion is a §5.1 error).
//
// The bug (#1245 / BAD_ACCEPT): both span families are null-declaration Named
// builtins with identical struct layout {ptr,i32,i32}; checkAssignable's
// conservative fallback for unrecognised builtin pairs silently accepted
// ReadOnlyArraySpan<T>→ArraySpan<T> and ReadOnlySpan<T>→Span<T>, allowing
// code to write through a read-only view.
//
// Fix: an explicit rejection in checkAssignable (ExprTypecheck.cpp) for the
// two cross-mutability pairs before the variantGenericAssignable fallthrough.
//
// Tests:
//   §4.15/#1245 RO→writable ArraySpan call arg rejected
//   §4.14/#1245 RO→writable Span call arg rejected
//   §4.15/#1245 RO→writable ArraySpan assignment rejected
//   §4.14/#1245 RO→writable Span assignment rejected
//   §4.15 regression: ArraySpan→ArraySpan call arg accepted (same type)
//   §4.15 regression: ReadOnlyArraySpan→ReadOnlyArraySpan call arg accepted
//   §4.14 regression: ReadOnlySpan→ReadOnlySpan call arg accepted
//   §4.15 regression: ReadOnlyArraySpan read-only surface (Length/indexer/for-in)
//   §4.15 regression: ArraySpan writable surface (indexer-set) still compiles

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Returns true iff the source is rejected at compile-time with at least one
// diagnostic error (i.e. no exe produced).
bool rejectedAtCompile(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// Returns the exit code from an AOT run; -999 on compile/link failure.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ===========================================================================
// §4.15 / #1245 — ReadOnlyArraySpan<T> passed where ArraySpan<T> expected
// ===========================================================================

// §4.15 / #1245: passing a ReadOnlyArraySpan<int> where an ArraySpan<int>
// parameter is declared must be a compile-time §5.1 error.
// Pre-fix: silently accepted; wrote through the read-only borrow.
BEZEL_TEST(span_readonly_assignability_1245,
           readonly_array_span_arg_where_writable_expected_rejects_spec_4_15_issue_1245) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C "
        "  public static method Mutate(sp: ArraySpan<int>) "
        "    sp[0] := 77; "
        "  end Mutate; "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); arr.Add(0); "
        "    var ro := arr.AsReadOnlySpan(); "
        "    C.Mutate(ro); "          // ReadOnlyArraySpan passed as ArraySpan → §5.1
        "    return arr[0]; "
        "  end Main; end C;"));
}

// ===========================================================================
// §4.14 / #1245 — ReadOnlySpan<T> passed where Span<T> expected
// ===========================================================================

// §4.14 / #1245: passing a ReadOnlySpan<int> where a Span<int> parameter is
// declared must be a compile-time §5.1 error.
BEZEL_TEST(span_readonly_assignability_1245,
           readonly_span_arg_where_writable_expected_rejects_spec_4_14_issue_1245) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; import System.Serialization; class C "
        "  public static method Mutate(sp: Span<int>) "
        "    sp[0] := 77; "
        "  end Mutate; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(1); "
        "    var ro := buf.AsReadOnlySpan(); "
        "    C.Mutate(ro); "          // ReadOnlySpan passed as Span → §5.1
        "    return 0; "
        "  end Main; end C;"));
}

// ===========================================================================
// §4.15 / #1245 — var sp: ArraySpan<T> := ro (ReadOnlyArraySpan)
// ===========================================================================

// §4.15 / #1245: initialising an ArraySpan<int> local from a
// ReadOnlyArraySpan<int> value is a §5.1 error (assignment form).
BEZEL_TEST(span_readonly_assignability_1245,
           readonly_array_span_init_where_writable_expected_rejects_spec_4_15_issue_1245) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var arr := new Array<int>(); arr.Add(1); "
        "  var ro := arr.AsReadOnlySpan(); "
        "  var sp: ArraySpan<int> := ro; "   // §5.1: RO→writable
        "  return 0; "
        "end Main; end C;"));
}

// ===========================================================================
// §4.14 / #1245 — var sp: Span<T> := ro (ReadOnlySpan)
// ===========================================================================

// §4.14 / #1245: initialising a Span<int> local from a ReadOnlySpan<int>
// value is a §5.1 error (assignment form).
BEZEL_TEST(span_readonly_assignability_1245,
           readonly_span_init_where_writable_expected_rejects_spec_4_14_issue_1245) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; import System.Serialization; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(1); "
        "  var ro := buf.AsReadOnlySpan(); "
        "  var sp: Span<int> := ro; "        // §5.1: RO→writable
        "  return 0; "
        "end Main; end C;"));
}

// ===========================================================================
// Regressions — must still compile (§4.14/§4.15)
// ===========================================================================

// §4.15 regression (#1245): passing an ArraySpan<int> where ArraySpan<int>
// is expected (same type) must still compile and run correctly.
BEZEL_TEST(span_readonly_assignability_1245,
           writable_array_span_to_same_type_still_accepted_spec_4_15_issue_1245) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static method SumSpan(sp: ArraySpan<int>): int "
        "    var t := 0; "
        "    for var x in sp do t := t + x; end; "
        "    return t; "
        "  end SumSpan; "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); arr.Add(4); arr.Add(5); "
        "    return SumSpan(arr.AsSpan()); "   // ArraySpan → ArraySpan OK
        "  end Main; end C;"), 9);
}

// §4.15 regression (#1245): passing a ReadOnlyArraySpan<int> where
// ReadOnlyArraySpan<int> is expected (same type) must still compile.
BEZEL_TEST(span_readonly_assignability_1245,
           readonly_array_span_to_same_type_still_accepted_spec_4_15_issue_1245) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static method LenRO(sp: ReadOnlyArraySpan<int>): int "
        "    return sp.Length; "
        "  end LenRO; "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); arr.Add(1); arr.Add(2); arr.Add(3); "
        "    var ro := arr.AsReadOnlySpan(); "
        "    return LenRO(ro); "              // ReadOnlyArraySpan → ReadOnlyArraySpan OK
        "  end Main; end C;"), 3);
}

// §4.14 regression (#1245): passing a ReadOnlySpan<int> where
// ReadOnlySpan<int> is expected (same type) must still compile.
BEZEL_TEST(span_readonly_assignability_1245,
           readonly_span_to_same_type_still_accepted_spec_4_14_issue_1245) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; class C "
        "  private static method LenRO(sp: ReadOnlySpan<int>): int "
        "    return sp.Length; "
        "  end LenRO; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(7); "
        "    return LenRO(buf.AsReadOnlySpan()); "  // ReadOnlySpan → ReadOnlySpan OK
        "  end Main; end C;"), 7);
}

// §4.15 regression (#1245): a ReadOnlyArraySpan<int> local used read-only
// (Length / indexed-get / for-in) must still compile and produce correct values.
BEZEL_TEST(span_readonly_assignability_1245,
           readonly_array_span_read_only_surface_still_works_spec_4_15_issue_1245) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(10); arr.Add(20); arr.Add(30); "
        "  var ro := arr.AsReadOnlySpan(); "
        "  var sum := 0; "
        "  for var x in ro do sum := sum + x; end; "   // for-in
        "  return sum + ro.Length + ro[0]; "            // Length + indexer-get
        "end Main; end C;"), 73);   // 60 + 3 + 10
}

// §4.15 regression (#1245): an ArraySpan<int> writable local (indexer-set,
// for-in) must still compile correctly.
BEZEL_TEST(span_readonly_assignability_1245,
           writable_array_span_mutate_surface_still_works_spec_4_15_issue_1245) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var arr := new Array<int>(); arr.Add(0); arr.Add(0); "
        "  var sp := arr.AsSpan(); "
        "  sp[0] := 11; sp[1] := 22; "     // writable indexer-set still OK
        "  return arr.Get(0) + arr.Get(1); "
        "end Main; end C;"), 33);
}
