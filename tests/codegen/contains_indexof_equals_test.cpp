// Codegen — Array<T>/OwningArray<T>.Contains(item) and .IndexOf(item) match via
// the element type's EQUALITY, not just pointer identity (issue #424).
//
// stdlib §4.4/§4.5: `Contains(item): bool` / `IndexOf(item): int` find the
// first matching element. §25.2: "collections … use `Equals` rather than `=`."
// §8.3: a class defaults to reference equality but may OVERRIDE `Equals`; a
// positional record defaults to field-by-field value equality; a primitive
// uses value comparison. §4.6: strings compare by content. §8.4: for classes,
// `=` always invokes `Equals`.
//
// The query argument is a PURE BORROW (§4.4/§4.5): Contains/IndexOf only
// compare — they do not remove or dispose any element. An OwningArray element
// that matches is NOT disposed (unlike Remove). The query itself is never
// consumed by the call.
//
// Pre-fix: record element types caused SIGILL (exit 132) because the old code
// path tried an ICmp on a struct pointer, which is illegal IR. Scalar/string
// element types also regressed to pointer-identity semantics.

#include "support/aot_result_asserts.hpp"   // fix #3895: failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// -----------------------------------------------------------------------
// Contains — record element (the §424 reproducer)
// -----------------------------------------------------------------------

// §8.3/§25.2: a record element matches by field-by-field structural equality.
// Pre-fix: compiler crashed (SIGILL, exit 132). Pt(3,4) stored; Contains with
// a distinct equal-value Pt(3,4) must return true (exit 1).
BEZEL_TEST(contains_indexof_equals_4_4, record_contains_present) {
    BEZEL_EXPECT_EQ(run(
        "module T; "
        "public record P(X: int, Y: int) end P; "
        "public class Program public static method Main(): int "
        "  using var a := new Array<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  if a.Contains(new P(3, 4)) then return 1; end; "
        "  return 0; "
        "end Main; end Program;"), 1);
}

// §8.3/§25.2: record element absent — Contains returns false (exit 0).
BEZEL_TEST(contains_indexof_equals_4_4, record_contains_absent) {
    BEZEL_EXPECT_EQ(run(
        "module T; "
        "public record P(X: int, Y: int) end P; "
        "public class Program public static method Main(): int "
        "  using var a := new Array<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  if a.Contains(new P(5, 6)) then return 77; end; "
        "  return 0; "
        "end Main; end Program;"), 0);
}

// -----------------------------------------------------------------------
// IndexOf — record element (the §424 reproducer)
// -----------------------------------------------------------------------

// §8.3/§25.2: record element present — IndexOf returns the correct index (1).
// Pre-fix: compiler crashed (SIGILL, exit 132).
BEZEL_TEST(contains_indexof_equals_4_4, record_indexof_present) {
    BEZEL_EXPECT_EQ(run(
        "module T; "
        "public record P(X: int, Y: int) end P; "
        "public class Program public static method Main(): int "
        "  using var a := new Array<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  return a.IndexOf(new P(3, 4)); "
        "end Main; end Program;"), 1);
}

// §8.3/§25.2: record element absent — IndexOf returns -1.
BEZEL_TEST(contains_indexof_equals_4_4, record_indexof_absent) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module T; "
        "public record P(X: int, Y: int) end P; "
        "public class Program public static method Main(): int "
        "  using var a := new Array<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  Console.WriteLine(a.IndexOf(new P(5, 6))); return 0; "
        "end Main; end Program;"), "-1");
}

// -----------------------------------------------------------------------
// Contains / IndexOf — class element with user Equals override (§8.4)
// -----------------------------------------------------------------------

// §8.4/§25.2: a class with user Equals override: Contains matches an equal-
// but-distinct instance via Equals (not reference identity).
BEZEL_TEST(contains_indexof_equals_4_4, class_user_equals_contains_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is Box then var b := other as Box; return b!.X = X; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => X; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b1 := new Box(1); using var b2 := new Box(2); "
        "  var a := new Array<Box>(); a.Add(b1); a.Add(b2); "
        "  using var query := new Box(2); "
        "  var found := a.Contains(query); "
        "  a.Dispose(); "
        "  if found then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4/§25.2: IndexOf with user Equals — returns correct index.
BEZEL_TEST(contains_indexof_equals_4_4, class_user_equals_indexof_correct_index) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is Box then var b := other as Box; return b!.X = X; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => X; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b1 := new Box(1); using var b2 := new Box(2); "
        "  var a := new Array<Box>(); a.Add(b1); a.Add(b2); "
        "  using var query := new Box(2); "
        "  var idx := a.IndexOf(query); "
        "  a.Dispose(); "
        "  return idx; "
        "end Main; end C;"), 1);
}

// -----------------------------------------------------------------------
// Contains / IndexOf — scalar (int) element regression
// -----------------------------------------------------------------------

// §8.3: scalar (int) element — Contains still works after the fix (regression).
BEZEL_TEST(contains_indexof_equals_4_4, scalar_int_contains_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  if a.Contains(20) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3: scalar absent — Contains returns false.
BEZEL_TEST(contains_indexof_equals_4_4, scalar_int_contains_absent) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  if a.Contains(99) then return 77; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §8.3: scalar (int) element — IndexOf returns correct index (regression).
BEZEL_TEST(contains_indexof_equals_4_4, scalar_int_indexof_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  return a.IndexOf(20); "
        "end Main; end C;"), 1);
}

// §8.3: scalar absent — IndexOf returns -1.
BEZEL_TEST(contains_indexof_equals_4_4, scalar_int_indexof_absent) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  Console.WriteLine(a.IndexOf(99)); return 0; "
        "end Main; end C;"), "-1");
}

// -----------------------------------------------------------------------
// Contains / IndexOf — string element regression
// -----------------------------------------------------------------------

// §4.6: string element — Contains matches by CONTENT (regression).
BEZEL_TEST(contains_indexof_equals_4_6, string_contains_by_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"ab\"); a.Add(\"cd\"); "
        "  if a.Contains(\"a\" + \"b\") then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6: string element — IndexOf returns correct index by content.
BEZEL_TEST(contains_indexof_equals_4_6, string_indexof_by_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"ab\"); a.Add(\"cd\"); "
        "  return a.IndexOf(\"c\" + \"d\"); "
        "end Main; end C;"), 1);
}

// -----------------------------------------------------------------------
// OwningArray — record element (issue #424 sibling: OwningArray variant)
// -----------------------------------------------------------------------

// §4.5/§14.4/#1327 v0.165: OwningArray<P> with a record element type is NOW
// rejected at sema (stdlib §4.2: owned collections require class/interface
// elements; records are value types with no disposal obligation). These tests
// were converted from behavioral to rejection tests when #1327 landed.
// The behavioral Contains/IndexOf for class elements with custom Equals is
// covered by owned_array_contains_query_is_pure_borrow below.
BEZEL_TEST(contains_indexof_equals_4_5, owned_array_record_contains_present) {
    auto r = runAot(
        "module T; "
        "public record P(X: int, Y: int) end P; "
        "public class Program public static method Main(): int "
        "  using var a := new OwningArray<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  if a.Contains(new P(3, 4)) then return 1; end; "
        "  return 0; "
        "end Main; end Program;");
    // fix #3895 (issue #3892): distinguishes a genuine §4.5/§14.4 rejection
    // from a runtime crash/signal, which a bare `hasErrors()`/`!ran` pair
    // cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// §4.5/§14.4/#1327 v0.165: OwningArray<P> with record element rejected at sema.
BEZEL_TEST(contains_indexof_equals_4_5, owned_array_record_indexof_present) {
    auto r = runAot(
        "module T; "
        "public record P(X: int, Y: int) end P; "
        "public class Program public static method Main(): int "
        "  using var a := new OwningArray<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  return a.IndexOf(new P(3, 4)); "
        "end Main; end Program;");
    // fix #3895 (issue #3892): distinguishes a genuine §4.5/§14.4 rejection
    // from a runtime crash/signal, which a bare `hasErrors()`/`!ran` pair
    // cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// §4.5: OwningArray<R> with class element: Contains query is a PURE BORROW —
// the destructor counter must NOT be incremented by Contains itself. After the
// scope-end the three stored elements and the query are disposed exactly once.
// Encoded: <counter after Contains>*10 + <final counter>. Contains disposes
// nothing → 0 during the call. scope-end: 3 stored + 1 query = 4. Expect 04.
BEZEL_TEST(contains_indexof_equals_4_5, owned_array_contains_query_is_pure_borrow) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is R then var b := other as R; return b!.X = X; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => X; "
        "  destructor R._n := R._n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var afterContains := -1; "
        "  do "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(move (new R(1))); "
        "    a.Add(move (new R(2))); "
        "    a.Add(move (new R(3))); "
        "    using var query := new R(2); "
        "    var found := a.Contains(query); "
        "    afterContains := R._n; "  // Contains disposes nothing → 0
        "  end; "  // 3 stored + 1 query disposed at scope-end → 4
        "  return afterContains * 10 + R._n; "
        "end Main; end C;"), 4);
}

// §4.5: OwningArray<R> with class element: IndexOf query is a PURE BORROW —
// same disposal accounting as Contains above. Expect 04.
BEZEL_TEST(contains_indexof_equals_4_5, owned_array_indexof_query_is_pure_borrow) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is R then var b := other as R; return b!.X = X; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => X; "
        "  destructor R._n := R._n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var afterIndexOf := -1; "
        "  do "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(move (new R(1))); "
        "    a.Add(move (new R(2))); "
        "    a.Add(move (new R(3))); "
        "    using var query := new R(2); "
        "    var idx := a.IndexOf(query); "
        "    afterIndexOf := R._n; "  // IndexOf disposes nothing → 0
        "  end; "  // 3 stored + 1 query disposed at scope-end → 4
        "  return afterIndexOf * 10 + R._n; "
        "end Main; end C;"), 4);
}
