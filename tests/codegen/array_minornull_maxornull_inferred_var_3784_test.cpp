// Spec-derived regression tests for #3784 (v0.286): `Array<T>.MinOrNull()` /
// `.MaxOrNull()` at a class or interface element type, with the receiving
// `var` having NO explicit type annotation. stdlib §5.1 declares
//
//     public extension IEnumerable<T> where T: IComparable<T>
//       public method Min(): T;
//       public method Max(): T;
//       public method MinOrNull(): T?;       // null if empty
//       public method MaxOrNull(): T?;       // null if empty
//     end;
//
// `T?` is the no-selector result. The existing #2783 tests (and the
// downstream codegen array arm at `LlvmBackend_Calls.cpp:~3753` from #2783,
// widened by #3764) already run that array arm and produce the correct
// boxed-class value once the call is REACHED. The defect was at the
// type-resolution site: when the receiving `var` had no explicit `: T?`
// annotation, the *call's own `exprType`* was never set (the existing
// `FirstOrNull`/`LastOrNull`/`SingleOrNull` arms at ExprTypecheck_Calls.cpp
// ~L6431 / ~L4986 set `T?` explicitly, but MinOrNull/MaxOrNull were never
// added to that disjunction). With `exprType` null, `lowerLocalDecl`
// (`LlvmBackend_Setup.cpp:~14769`) fell into the §2434 silent-degrade
// branch and emitted the
//
//     "internal: lowerLocalDecl's initializer expression failed to lower
//      to a value for this local's slot (degraded to a zero-initialized
//      value) — #2434"
//
// warning, the inferred local's slot defaulted to i32, and the next
// `mn!.Value` read produced zero. This file's tests pin the FIX: thread
// the array's element type through `MinOrNull`/`MaxOrNull`'s result-type
// derivation BEFORE the var-decl's type-inference fallback to #2434, so
// an un-annotated `var mn := arr.MinOrNull();` carries the right `T?`
// type into the slot-typing site.
//
// All rows use runAotChecked (ASan/LSan per test) so a wrong slot layout
// surfaces here rather than as a value coincidence.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #3784 / stdlib §5.1: `var mn := arr<Box>.MinOrNull()` — no explicit
// annotation on the receiving var. The fix threads the element type
// through the call's exprType as `Box?`, so the inferred local carries
// a pointer-typed slot (mirroring `FirstOrNull`/`LastOrNull`/
// `SingleOrNull` at ~L6431), and `mn!.Value` reads the actual Box, not
// the zero-init i32 the pre-fix #2434 degrade substituted. Discriminator
// 1 means "the program read the right `Value=1` from the borrowed Box";
// the pre-fix path printed `0`.
//
// Without the fix: lowerLocalDecl falls into the #2434 warning branch
// (an i32 slot), `mn!.Value` reads garbage, the program likely
// crashes or prints 0.
BEZEL_TEST(array_minornull_maxornull_inferred_var_3784,
           min_ornull_inferred_var_box_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System; "
        "class Box: IComparable<Box> "
        "  public Value: int := 0; "
        "  public constructor(v: int) Value := v; end; "
        "  public method CompareTo(other: Box): int "
        "    return Value - other.Value; "
        "  end CompareTo; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Box(3); "
        "    using var b := new Box(1); "
        "    var arr: Array<Box> := [a, b]; "
        "    var mn := arr.MinOrNull(); "
        "    var v := mn!.Value; "
        "    Console.WriteLine(v); "
        "    return 0; "
        "  end Main; "
        "end C; "), 0);
}

// #3784 / stdlib §5.1: MaxOrNull() with no annotation, sibling of the
// min row above. Same expected value-source path; discriminator 1
// because the max of {3, 1} is the Box with Value=3.
//
// Without the fix: same i32-slot degrade, garbage `mx!.Value`.
BEZEL_TEST(array_minornull_maxornull_inferred_var_3784,
           max_ornull_inferred_var_box_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System; "
        "class Box: IComparable<Box> "
        "  public Value: int := 0; "
        "  public constructor(v: int) Value := v; end; "
        "  public method CompareTo(other: Box): int "
        "    return Value - other.Value; "
        "  end CompareTo; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Box(1); "
        "    using var b := new Box(5); "
        "    var arr: Array<Box> := [a, b]; "
        "    var mx := arr.MaxOrNull(); "
        "    var v := mx!.Value; "
        "    Console.WriteLine(v); "
        "    return 0; "
        "  end Main; "
        "end C; "), 0);
}

// #3784 / stdlib §5.1: inferred-var `MinOrNull` on an INTERFACE element
// type. The issue body calls this out specifically: "Also reproduces at
// an INTERFACE element type (`IQux: IComparable<IQux>`), same shape:
// inferred `var mn := arr.MinOrNull();` degrades; `var mn: IQux? :=
// arr.MinOrNull();` works." Discriminator 13 mirrors the existing
// #2783 IQux test's packing (Min*10 + Max on Rank).
//
// Without the fix: i32-slot degrade, garbage read.
BEZEL_TEST(array_minornull_maxornull_inferred_var_3784,
           min_max_ornull_inferred_var_iface_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System; "
        "public interface IQux: IComparable<IQux> "
        "  method Rank(): int; "
        "end IQux; "
        "class QNode: IQux "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IQux): int => _r - other.Rank(); "
        "end QNode; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new QNode(3); "
        "    using var b := new QNode(1); "
        "    var arr: Array<IQux> := [a, b]; "
        "    var mn := arr.MinOrNull(); "
        "    var mx := arr.MaxOrNull(); "
        "    if mn = null then return 1; end; "
        "    if mx = null then return 2; end; "
        "    return mn!.Rank() * 10 + mx!.Rank(); "
        "  end Main; "
        "end C; "), 13);
}

// #3784 / stdlib §5.1: lazy-chain receiver (`arr.Where(pred).MinOrNull()`)
// — the issue body explicitly records "Reproduces identically on a
// lazy-chain-materialized receiver". Without the fix the lazy-enumerable
// arm at ~L4986 also lacks MinOrNull/MaxOrNull typing, so the same
// #2434 degrade fires through the materialized snapshot. Discriminator 1
// means "the program read Value=1 from the min Box that survived the
// filter".
//
// Without the fix: same i32-slot degrade on the call to .MinOrNull()
// over the lazy IEnumerable<Box>; `mn!.Value` reads garbage.
BEZEL_TEST(array_minornull_maxornull_inferred_var_3784,
           min_ornull_inferred_var_lazy_chain) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System; "
        "class Box: IComparable<Box> "
        "  public Value: int := 0; "
        "  public constructor(v: int) Value := v; end; "
        "  public method CompareTo(other: Box): int "
        "    return Value - other.Value; "
        "  end CompareTo; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Box(3); "
        "    using var b := new Box(1); "
        "    var arr: Array<Box> := [a, b]; "
        "    var mn := arr.Where(lambda(x) => x.Value > 0).MinOrNull(); "
        "    var v := mn!.Value; "
        "    Console.WriteLine(v); "
        "    return 0; "
        "  end Main; "
        "end C; "), 0);
}

// #3784 / stdlib §5.1: control row — an INT element `var mn := arr.IntOrNull`
// still passes, the same shape the existing #2783 test exercises but
// without the `: int?` annotation. Confirms the new arm didn't break
// the primitive-element path; discriminator 25 mirrors #2783's
// `Min*10 + Max` packing.
//
// Without the fix: would FAIL the same way (the same #2434 degrade).
// Pre-fix path: int element type — would still degrade because the
// FirstOrNull/LastOrNull/SingleOrNull arms don't cover MinOrNull either.
// Post-fix path: returns the right int? and the print reads 25.
BEZEL_TEST(array_minornull_maxornull_inferred_var_3784,
           min_max_ornull_inferred_var_int_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Main(): int "
        "    var arr: Array<int> := new Array<int>(3); "
        "    arr.Add(5); "
        "    arr.Add(2); "
        "    var mn := arr.MinOrNull(); "
        "    var mx := arr.MaxOrNull(); "
        "    if mn = null then return 1; end; "
        "    if mx = null then return 2; end; "
        "    return mn! * 10 + mx!; "
        "  end Main; "
        "end C; "), 25);
}
