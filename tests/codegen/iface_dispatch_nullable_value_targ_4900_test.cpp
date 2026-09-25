// Regression tests for issue #4900 — §13 (interfaces / interface dispatch),
// §14.1 ("Generic declarations"), §7 ("Nullable types") and §7.10 ("Generics
// and nullability"), v0.307.
//
// THE DEFECT. A call through a generic interface instantiated at a nullable
// value type — `var i: IId<int?> := h; i.Id(5)` where `h: Holder<int?>` —
// returned layout-dependent garbage (1 / 16 / 193679). Silent wrong code.
//
// MECHANISM. The itable slot for `Holder<int?>` is the specialization that
// takes the tagged `{i8, i32}` carrier for the bare `v: T` formal (the same
// carrier emitGenericSpecialization builds for the direct call, #4830). The
// interface-dispatch arm of emitMethodCall (LlvmBackend_Calls2.cpp) passed the
// argument as the bare payload (`i32 5`) and then retyped the call from the
// argument types (#1183's concrete-fnTy rebuild), so the callee read an
// `{i8, i32}` out of an `i32` register.
//
// THE FIX. The interface arm shapes each argument for its interface formal
// under the active substitution (the receiver's `IId<int?>` binds the
// interface's `T`) with wrapArgForNullableValueFormal before the rebuild.
//
// Rows assert exact stdout; a null payload is printed through `?? -1`.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// ─── The issue's own repro ────────────────────────────────────────────────
BEZEL_TEST(iface_dispatch_nullable_value_targ_4900, issue_repro_id_5) {
    JitResult r = runAotChecked(
        "module M; "
        "public interface IId<T> "
        "  method Id(v: T): T; "
        "end IId; "
        "public class Holder<T>: IId<T> "
        "  public constructor() end constructor; "
        "  public method Id(v: T): T "
        "    return v; "
        "  end Id; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    using var h := new Holder<int?>(); "
        "    var i: IId<int?> := h; "
        "    Console.WriteLine(i.Id(5) ?? -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// ─── Argument shapes and payload types through the interface ──────────────
// literal, null literal, null / present int? variable (the latter two also
// exercise #4899's carrier restore on the interface path), a bool-returning
// method, long? and bool? payloads, and an IId<int> control.
BEZEL_TEST(iface_dispatch_nullable_value_targ_4900, arg_shapes_and_payloads) {
    JitResult r = runAotChecked(
        "module M; "
        "public interface IId<T> "
        "  method Id(v: T): T; "
        "  method IsNull(v: T): bool; "
        "end IId; "
        "public class Holder<T>: IId<T> "
        "  public constructor() end constructor; "
        "  public method Id(v: T): T "
        "    return v; "
        "  end Id; "
        "  public method IsNull(v: T): bool "
        "    return v = null; "
        "  end IsNull; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    using var h := new Holder<int?>(); "
        "    var i: IId<int?> := h; "
        "    var x: int? := null; "
        "    var y: int? := 7; "
        "    Console.WriteLine(i.Id(5) ?? -1); "
        "    Console.WriteLine(i.Id(null) ?? -1); "
        "    Console.WriteLine(i.Id(x) ?? -1); "
        "    Console.WriteLine(i.Id(y) ?? -1); "
        "    Console.WriteLine(i.IsNull(x)); "
        "    Console.WriteLine(i.IsNull(3)); "
        "    using var hl := new Holder<long?>(); "
        "    var il: IId<long?> := hl; "
        "    Console.WriteLine(il.Id(9) ?? -1); "
        "    Console.WriteLine(il.IsNull(null)); "
        "    using var hb := new Holder<bool?>(); "
        "    var ib: IId<bool?> := hb; "
        "    Console.WriteLine(ib.Id(true) ?? false); "
        "    Console.WriteLine(ib.IsNull(null)); "
        "    using var hi := new Holder<int>(); "
        "    var ii: IId<int> := hi; "
        "    Console.WriteLine(ii.Id(4)); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "5\n-1\n-1\n7\nTrue\nFalse\n9\nTrue\nTrue\nTrue\n4\n");
}

// ─── Loop-amplified (50×): an implementor is allocated each iteration ─────
BEZEL_TEST(iface_dispatch_nullable_value_targ_4900, loop_amplified_50) {
    JitResult r = runAotChecked(
        "module M; "
        "public interface IId<T> "
        "  method Id(v: T): T; "
        "end IId; "
        "public class Holder<T>: IId<T> "
        "  public constructor() end constructor; "
        "  public method Id(v: T): T "
        "    return v; "
        "  end Id; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    var nulls := 0; "
        "    var sum := 0; "
        "    for var i in 0..<50 do "
        "      using var h := new Holder<int?>(); "
        "      var d: IId<int?> := h; "
        "      var x: int? := null; "
        "      if i mod 2 = 1 then x := i; end; "
        "      if d.Id(x) = null then nulls := nulls + 1; end; "
        "      sum := sum + (d.Id(i) ?? 0); "
        "    end; "
        "    Console.WriteLine(nulls); "
        "    Console.WriteLine(sum); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "25\n1225\n");
}
