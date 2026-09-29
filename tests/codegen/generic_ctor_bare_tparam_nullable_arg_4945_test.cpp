// Regression tests for issue #4945 — §14.1 ("Generic declarations"), §7
// ("Nullable types") and §7.10 ("Generics and nullability"), v0.307.
//
// THE DEFECT. The constructor sibling of #4899: a null `int?` VARIABLE passed
// to a bare `v: T` CONSTRUCTOR parameter of a class instantiated at
// `T := int?` (`new Box<int?>(x)` with `x = null`) arrived as a PRESENT `0`.
// Silent wrong code: `b.Get() ?? -1` printed 0.
//
// MECHANISM. `new` sites lower their arguments under the ENCLOSING
// substitution (#597), so at argument time the bare `T` formal lowers to the
// i32 placeholder and buildOrderedArgs' §7.4 narrowed-read unwrap (#1191)
// strips the `{i8, i32}` tag. #4899 records those unwraps but only the
// method-call path (lowerCallDispatch) restored them.
//
// THE FIX. Each `new` site (LlvmBackend_Exprs2.cpp) restores the recorded
// carriers under the constructed type's substitution right after
// buildOrderedArgs (restoreCtorBareTParamCarriers).
//
// Rows assert exact stdout; a null payload is printed through `?? -1`.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// ─── The issue's own repro ────────────────────────────────────────────────
BEZEL_TEST(generic_ctor_bare_tparam_nullable_arg_4945, issue_repro_box_null_var) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Get(): T => _v; "
        "end Box; "
        "public class C "
        "  public static method Main() "
        "    var x: int? := null; "
        "    var y: int? := 4; "
        "    using var b := new Box<int?>(x); "
        "    using var c := new Box<int?>(y); "
        "    Console.WriteLine(b.Get() ?? -1); "
        "    Console.WriteLine(c.Get() ?? -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n4\n");
}

// ─── Two bare-T formals, mixed null/present, plus a null test in the ctor ──
// and a narrowed argument into `Box<int>` (the §7.4 unwrap must be kept).
BEZEL_TEST(generic_ctor_bare_tparam_nullable_arg_4945, two_params_and_controls) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Pair<T> "
        "  private _a: T; "
        "  private _b: T; "
        "  private _aNull: bool; "
        "  public constructor(a: T, b: T) "
        "    _a := a; _b := b; _aNull := a = null; "
        "  end constructor; "
        "  public method A(): T => _a; "
        "  public method B(): T => _b; "
        "  public method ANull(): bool => _aNull; "
        "end Pair; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Get(): T => _v; "
        "end Box; "
        "public class C "
        "  public static method Main() "
        "    var n: int? := null; "
        "    var p: int? := 6; "
        "    using var q := new Pair<int?>(n, p); "
        "    Console.WriteLine(q.A() ?? -1); "
        "    Console.WriteLine(q.B() ?? -1); "
        "    Console.WriteLine(q.ANull()); "
        "    using var q2 := new Pair<int?>(p, n); "
        "    Console.WriteLine(q2.A() ?? -1); "
        "    Console.WriteLine(q2.B() ?? -1); "
        "    Console.WriteLine(q2.ANull()); "
        "    var z: int? := 8; "
        "    if z <> null then "
        "      using var bi := new Box<int>(z); "
        "      Console.WriteLine(bi.Get()); "
        "      using var bn := new Box<int?>(z); "
        "      Console.WriteLine(bn.Get() ?? -1); "
        "    end; "
        "    var l: long? := null; "
        "    using var bl := new Box<long?>(l); "
        "    Console.WriteLine(bl.Get() ?? -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n6\nTrue\n6\n-1\nFalse\n8\n8\n-1\n");
}

// ─── A `new` nested in a method argument, and a method call nested in a ───
// constructor argument: neither's recorded entries may leak into the other.
BEZEL_TEST(generic_ctor_bare_tparam_nullable_arg_4945, nested_new_and_call) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Get(): T => _v; "
        "end Box; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method IdB(v: T): T "
        "    return v; "
        "  end IdB; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    var x: int? := null; "
        "    using var h := new Holder<int?>(); "
        "    using var b := new Box<int?>(h.IdB(x)); "
        "    Console.WriteLine(b.Get() ?? -1); "
        "    using var b2 := new Box<int?>(x); "
        "    Console.WriteLine(h.IdB(b2.Get()) ?? -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n-1\n");
}

// ─── Loop-amplified (50×): a Box is allocated each iteration ──────────────
BEZEL_TEST(generic_ctor_bare_tparam_nullable_arg_4945, loop_amplified_50) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Get(): T => _v; "
        "end Box; "
        "public class C "
        "  public static method Main() "
        "    var nulls := 0; "
        "    var sum := 0; "
        "    for var i in 0..<50 do "
        "      var x: int? := null; "
        "      if i mod 2 = 1 then x := i; end; "
        "      using var b := new Box<int?>(x); "
        "      if b.Get() = null then nulls := nulls + 1; end; "
        "      sum := sum + (b.Get() ?? 0); "
        "    end; "
        "    Console.WriteLine(nulls); "
        "    Console.WriteLine(sum); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "25\n625\n");
}
