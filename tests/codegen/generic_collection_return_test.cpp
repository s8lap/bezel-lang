// Codegen — a generic method that returns a populated stdlib heap collection
// (issue #420). Two crashes regressed here:
//
//   [00] RUNTIME SEGFAULT. A generic method `Wrap<T>(item: T): owned Array<T>`
//        that builds a `new Array<T>()`, `Add`s an element, and `return move a;`,
//        bound at the call site by `using var a: Array<int> := Util.Wrap(7);`,
//        compiled cleanly but crashed in bz_array_free at the using-var
//        scope-end disposal (§28.3). The specialized monomorphized function
//        (emitGenericSpecialization, §14.1) was typed to return i32 instead of
//        the bare `bz_array_t*` pointer: lowerTypeRef(LlvmBackend_Layout.cpp)
//        returned i32 for ANY no-declaration Named type while a generic
//        substitution was active, clobbering the heap-collection lowering. The
//        bare pointer was truncated to 32 bits on the return/store and the
//        scope-end collection disposal loaded a corrupt pointer.
//
//   [01] COMPILER ABORT ("LLVM ERROR: Cannot emit physreg copy instruction").
//        `Console.WriteLine(a.Count)` on a heap-collection `Count` read
//        (stdlib §4.4) was passed to the console-write path with no Primitive
//        sema type, so the raw i32 length was GEP'd as a `bz_string*`. The
//        WriteLine lowering now falls back on the lowered value's LLVM type
//        (mirroring the string-interpolation path) and converts the integer.
//
// These are spec-valid programs: §14.1 generic methods, §28.8 owned-return /
// move, §28.3 using-var disposal, stdlib §4.4 Array<T> / §4.5 OwningArray<T>.
// Each test ran (segfault / compiler abort) before the fix and produces the
// spec-stated value after.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// [00] §14.1 / §28.8 / §28.3 / stdlib §4.4: a generic method returns an
// `owned Array<int>` populated with one element; the caller binds it with a
// `using var` and lets it dispose at scope end. Before the fix this segfaulted
// in bz_array_free on a 32-bit-truncated pointer. After: it disposes cleanly
// and returns 0. (Original finding [00] minimal repro.)
BEZEL_TEST(generic_coll_return_420, array_int_using_var_disposes_clean) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Util "
        "  public static method Wrap<T>(item: T): owned Array<T> "
        "    var a := new Array<T>(); "
        "    a.Add(item); "
        "    return a; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var a: Array<int> := Util.Wrap(7); "
        "  return 0; "
        "end Main; end C;"), 0);
}

// [00] stdlib §4.4: the returned array value is well-formed — its Count is the
// number of Adds. This reads through the monomorphized-return slot that was
// corrupt before the fix (the array stored into the using-var slot is the bare
// runtime pointer, so Count is 1).
BEZEL_TEST(generic_coll_return_420, array_int_count_after_return) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Util "
        "  public static method Wrap<T>(item: T): owned Array<T> "
        "    var a := new Array<T>(); "
        "    a.Add(item); "
        "    return a; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var a: Array<int> := Util.Wrap(7); "
        "  var n: int := a.Count; "
        "  return n; "
        "end Main; end C;"), 1);
}

// [00] stdlib §4.4: same shape with `Array<string>` (element type is a pointer,
// not a primitive int) — also segfaulted at scope-end before the fix. The
// element read confirms the returned array is intact: a[0].Length == 2 for "hi".
BEZEL_TEST(generic_coll_return_420, array_string_using_var_disposes_clean) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Util "
        "  public static method Wrap<T>(item: T): owned Array<T> "
        "    var a := new Array<T>(); "
        "    a.Add(item); "
        "    return a; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var a: Array<string> := Util.Wrap(\"hi\"); "
        "  return a[0].Length; "
        "end Main; end C;"), 2);
}

// [00] §28.8 / stdlib §4.5: an `owned OwningArray<T>` (T: class) returned from a
// generic method. The using-var scope-end disposal must cascade into the owned
// element exactly once (the element destructor bumps R._n). Before the fix the
// disposal loaded a corrupt pointer and crashed; after, R._n == 1.
BEZEL_TEST(generic_coll_return_420, owned_array_generic_return_disposes_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class Util "
        "  public static method WrapOne<T>(owned item: T): owned OwningArray<T> "
        "      where T: class "
        "    var a := new OwningArray<T>(); "
        "    a.Add(move item); "
        "    return a; "
        "  end WrapOne; "
        "end Util; "
        "class C public static method Main(): int "
        "  do "
        "    using var a: OwningArray<R> := Util.WrapOne(move (new R())); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 1);
}

// [01] stdlib §4.4 / §2.1: boxing-free console write of a heap-collection Count
// read. `Console.WriteLine(a.Count)` aborted codegen ("Cannot emit physreg copy
// instruction") before the fix because the untyped i32 length flowed into the
// console-write path as a string pointer. The program prints "1" and returns 0.
// (Original finding [01] minimal repro — no generic method required.)
BEZEL_TEST(generic_coll_return_420, writeline_array_count_compiles_runs) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(7); "
        "  Console.WriteLine(a.Count); "
        "  return 0; "
        "end Main; end C;"), 0);
}

// [01] combined with [00]: a generic method returns the array, then its Count is
// written directly to the console. This is the exact original finding [01] repro
// (generic Wrap + WriteLine(a.Count)) and exercises both fixed paths at once.
BEZEL_TEST(generic_coll_return_420, generic_return_then_writeline_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Util "
        "  public static method Wrap<T>(item: T): owned Array<T> "
        "    var a := new Array<T>(); "
        "    a.Add(item); "
        "    return a; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var a: Array<int> := Util.Wrap(7); "
        "  Console.WriteLine(a.Count); "
        "  return 0; "
        "end Main; end C;"), 0);
}

// Regression guard (§14.1): a user-defined generic class `Box<T>` round-trips a
// value through a `using var` and a generic field read. This path was never
// broken by issue #420 (the bug was specific to no-declaration stdlib heap
// collections) and must keep working — the lowerTypeRef fix only changes the
// fall-through for builtin/FFI names, leaving the bound-generic-parameter
// resolution (T -> i32 here) intact.
BEZEL_TEST(generic_coll_return_420, user_generic_box_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(7); "
        "  return b.value; "
        "end Main; end C;"), 7);
}
