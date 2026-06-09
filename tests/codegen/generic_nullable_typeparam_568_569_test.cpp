// Codegen — a generic method whose substituted return/element type involves a
// NULLABLE bare type-parameter (`T?` / `Array<T?>`), monomorphized at the call
// site with a concrete value type (issues #568, #569).
//
// §14.1 (generic monomorphization): a generic method body is monomorphized at
// each call site; the type parameter is substituted by the concrete argument.
// §7.1 (nullable value types): `T?` for a value-type T lowers to the tagged
// {i8 hasValue; T value} struct (null = tag 0). §4.4 (Array): a heap array stores
// its element bytes at a stride matching the element's lowered size.
//
// Root causes (before fix), one unifying defect — a bare nullable type-parameter
// (`T?`, a single-part Named TypeRef with no declaration) was not resolved to its
// concrete substituted type, so the nullable lowering/decode used the wrong
// representation:
//
//   #568: lowerTypeRef(`T?`) fell through to the generic-subst branch, which
//   resolved `T` → its concrete LLVM type and DROPPED the `?`, typing a `T?`
//   (T=int) return as plain i32. The monomorphized body's `return null` then
//   emitted `ret ptr null` against an i32 return type → IR-verify failure. (The
//   unspecialized template body had the same mismatch.)
//
//   #569: Array<T?>.Add resolved the element genericArg `T?` directly;
//   isNullableValueTy(`T?`) is false, so the bare value `v` was stored WITHOUT
//   wrapping it into the {i8, T} tagged struct, while collectionElemSize used the
//   nullable element's (larger) stride. The element bytes were laid out wrong →
//   `arr[0]!` decoded garbage (e.g. 0 instead of 42).
//
// After fix (codegen): a bare nullable type-parameter resolves through the active
// monomorphization binding (genericSubstSemaTy_, the #567 resolveTypeParamRef
// mechanism) to the concrete nullable value type — lowerTypeRef returns its
// {i8, T} struct (#568), ReturnStmt wraps `return null`/`return <value>` into it,
// and Array<T?>.Add wraps the bare element into it (#569). A specialized-vs-
// unspecialized record struct mismatch is reconciled via a layout-identical
// reinterpret.
//
// Each test ran (IR-verify failure / miscompile) before the fix and produces the
// spec-stated value after.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

bezel::test::JitResult jit(const std::string& prog) { return runJit(prog); }

}  // namespace

// ── #568 / §14.1 / §7.1 : generic method returning `T?` ───────────────────────

// RetNull<int>(): int? returns null. Before fix: IR-verify failure
// (`ret ptr null` vs i32). After: r = null → return 1.
BEZEL_TEST(codegen_generic_nullable_typeparam_568, ret_null_int_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method RetNull<T>(): T? where T: notnull "
        "    return null; "
        "  end RetNull; "
        "end Util; "
        "class C public static method Main(): int "
        "  var r: int? := Util.RetNull<int>(); "
        "  if r = null then return 1; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// RetVal<int>(5): int? returns a value. The `return x` (x:T) wraps the bare value
// into the {i8, T} tagged struct (tag=1). After: r! == 5.
BEZEL_TEST(codegen_generic_nullable_typeparam_568, ret_val_int_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method RetVal<T>(x: T): T? where T: notnull "
        "    return x; "
        "  end RetVal; "
        "end Util; "
        "class C public static method Main(): int "
        "  var r: int? := Util.RetVal<int>(5); "
        "  if r = null then return 99; end; "
        "  return r!; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// §7.1 / §9.4: T = a RECORD. The `T?` payload is the record struct; the
// specialized-vs-unspecialized record struct mismatch is reinterpreted. RetNull
// is null; RetVal returns the record value. After: x + y == 7.
BEZEL_TEST(codegen_generic_nullable_typeparam_568, ret_record_14_1) {
    auto r = jit(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Util "
        "  public static method RetNull<T>(): T? where T: notnull "
        "    return null; "
        "  end RetNull; "
        "  public static method RetVal<T>(x: T): T? where T: notnull "
        "    return x; "
        "  end RetVal; "
        "end Util; "
        "class C public static method Main(): int "
        "  var n: Pt? := Util.RetNull<Pt>(); "
        "  if n <> null then return 90; end; "
        "  var v: Pt? := Util.RetVal<Pt>(new Pt(3, 4)); "
        "  if v = null then return 91; end; "
        "  return v!.x + v!.y; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §14.2: two distinct instantiations from the SAME generic method — each gets its
// own monomorphized nullable lowering (int? and a record?). Both must decode.
BEZEL_TEST(codegen_generic_nullable_typeparam_568, ret_two_instantiations_14_2) {
    auto r = jit(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Util "
        "  public static method RetVal<T>(x: T): T? where T: notnull "
        "    return x; "
        "  end RetVal; "
        "end Util; "
        "class C public static method Main(): int "
        "  var ri: int? := Util.RetVal<int>(4); "
        "  var rp: Pt? := Util.RetVal<Pt>(new Pt(1, 2)); "
        "  if ri! = 4 and rp!.x + rp!.y = 3 then return 9; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// ── #569 / §14.1 / §4.4 : generic method returning `owned Array<T?>` ───────────

// Wrap<int>(42): owned Array<int?> — Add wraps the bare T into the tagged struct
// at the nullable stride; the caller's arr[0]! decodes the same struct.
// Before fix: arr[0]! == 0 (bare value stored at the wrong stride). After: 42.
BEZEL_TEST(codegen_generic_nullable_typeparam_569, wrap_array_int_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method Wrap<T>(v: T): owned Array<T?> where T: notnull "
        "    var arr := new Array<T?>(); "
        "    arr.Add(v); "
        "    return arr; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var arr: Array<int?> := Util.Wrap<int>(42); "
        "  return arr[0]!; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §4.4 / §9.4: T = a RECORD. The array element is the tagged {i8, Pt} struct;
// Add reinterprets the specialized record struct before wrapping. After: 7.
BEZEL_TEST(codegen_generic_nullable_typeparam_569, wrap_array_record_14_1) {
    auto r = jit(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Util "
        "  public static method Wrap<T>(v: T): owned Array<T?> where T: notnull "
        "    var arr := new Array<T?>(); "
        "    arr.Add(v); "
        "    return arr; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var arr: Array<Pt?> := Util.Wrap<Pt>(new Pt(3, 4)); "
        "  var e: Pt? := arr[0]; "
        "  if e = null then return 91; end; "
        "  return e!.x + e!.y; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §7.1: a NULL element added through the generic body decodes as null in the
// caller — the {i8 0, T 0} tag must survive the round trip.
BEZEL_TEST(codegen_generic_nullable_typeparam_569, wrap_array_null_element_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method WrapNull<T>(): owned Array<T?> where T: notnull "
        "    var arr := new Array<T?>(); "
        "    var n: T? := null; "
        "    arr.Add(n); "
        "    return arr; "
        "  end WrapNull; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var arr: Array<int?> := Util.WrapNull<int>(); "
        "  var e: int? := arr[0]; "
        "  if e = null then return 5; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// §4.4: a multi-element generic Array<T?> — Count and both decoded values must be
// correct (the nullable stride/decode is consistent across the array).
BEZEL_TEST(codegen_generic_nullable_typeparam_569, wrap_array_two_elements_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method Pair<T>(a: T, b: T): owned Array<T?> "
        "      where T: notnull "
        "    var arr := new Array<T?>(); "
        "    arr.Add(a); "
        "    arr.Add(b); "
        "    return arr; "
        "  end Pair; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var arr: Array<int?> := Util.Pair<int>(10, 20); "
        "  if arr.Count = 2 and arr[0]! = 10 and arr[1]! = 20 then return 8; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}
