// Regression tests for issue #4899 — §14.1 ("Generic declarations") / §7
// ("Nullable types") / §7.10 ("Generics and nullability"), v0.307.
//
// THE DEFECT. A null nullable-VALUE expression (a local `x: int?` holding
// `null`) passed to a BARE type-parameter formal `v: T`, where the call's
// substitution makes `T := int?` (`Holder<int?>`'s instance method, or a
// generic method called as `GB<int?>(x)`), arrived in the callee as a
// PRESENT `0`. Silent wrong code: `h.IdB(x) ?? -1` printed 0, `h.IsNull(x)`
// printed False.
//
// MECHANISM. `buildOrderedArgs` (LlvmBackend_Assign.cpp) applies the §7.4 /
// §12.7 narrowed-read unwrap (`unwrapNarrowedNullable`, #1191) to an argument
// whose formal is not itself a nullable value type. The callee's substitution
// is only installed AFTER argument lowering (`applyMethodGenericSubst` and the
// receiver-generic-args block in lowerCall), so at that point the bare `T`
// formal lowers to the i32 placeholder, the `{i8, i32}` carrier's payload type
// matches it, and the unwrap stripped the tag (`extractvalue …, 1`). The
// #4830 helper (`wrapArgForNullableValueFormal`) then re-wrapped the bare
// payload as PRESENT.
//
// THE FIX. `buildOrderedArgs` records each carrier it stripped for a bare
// type-parameter formal; once the call's substitution is installed, lowerCall
// restores the carrier for every such formal that resolves to a nullable
// value type (`restoreBareTParamNullableCarriers`). A formal that resolves to
// a NON-nullable type (`Holder<int>` with a narrowed `int?` argument) keeps
// the unwrap — the control rows below pin that.
//
// Every row asserts the exact stdout; a null payload is printed through
// `?? -1` so a lost tag reads as `0` and a correct null as `-1`.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// ─── The issue's own repro (block-bodied method + generic method) ─────────
BEZEL_TEST(generic_bare_tparam_nullable_arg_4899, issue_repro_block_body) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method IdB(v: T): T "
        "    return v; "
        "  end IdB; "
        "  public method IsNull(v: T): bool "
        "    return v = null; "
        "  end IsNull; "
        "end Holder; "
        "public class C "
        "  public static method GB<U>(v: U): U "
        "    return v; "
        "  end GB; "
        "  public static method Main() "
        "    using var h := new Holder<int?>(); "
        "    var x: int? := null; "
        "    Console.WriteLine(h.IdB(x) ?? -1); "
        "    Console.WriteLine(h.IsNull(x)); "
        "    Console.WriteLine(GB<int?>(x) ?? -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\nTrue\n-1\n");
}

// ─── Arrow-bodied (inlined) method and generic method ─────────────────────
// `=> v;` bodies go through emitInlineGenericMethod, not a specialization;
// the present row guards against over-restoring a present value.
BEZEL_TEST(generic_bare_tparam_nullable_arg_4899, arrow_body_null_and_present) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method IdA(v: T): T => v; "
        "end Holder; "
        "public class C "
        "  public static method GA<U>(v: U): U => v; "
        "  public static method Main() "
        "    using var h := new Holder<int?>(); "
        "    var x: int? := null; "
        "    var y: int? := 5; "
        "    Console.WriteLine(h.IdA(x) ?? -1); "
        "    Console.WriteLine(h.IdA(y) ?? -1); "
        "    Console.WriteLine(GA<int?>(x) ?? -1); "
        "    Console.WriteLine(GA<int?>(y) ?? -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n5\n-1\n5\n");
}

// ─── Result bound to an inferred / declared local ─────────────────────────
BEZEL_TEST(generic_bare_tparam_nullable_arg_4899, result_bound_to_local) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method IdA(v: T): T => v; "
        "  public method IdB(v: T): T "
        "    return v; "
        "  end IdB; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    using var h := new Holder<int?>(); "
        "    var x: int? := null; "
        "    var r := h.IdA(x); "
        "    Console.WriteLine(r = null); "
        "    var s := h.IdB(x); "
        "    Console.WriteLine(s = null); "
        "    var t: int? := h.IdB(x); "
        "    Console.WriteLine(t ?? -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\n-1\n");
}

// ─── Nested calls: the inner call's recorded entry must not leak into ─────
// the outer call's argument list (and vice versa).
BEZEL_TEST(generic_bare_tparam_nullable_arg_4899, nested_calls) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method IdA(v: T): T => v; "
        "  public method IdB(v: T): T "
        "    return v; "
        "  end IdB; "
        "  public method Pick(a: T, b: T): T "
        "    return b; "
        "  end Pick; "
        "end Holder; "
        "public class C "
        "  public static method GB<U>(v: U): U "
        "    return v; "
        "  end GB; "
        "  public static method Main() "
        "    using var h := new Holder<int?>(); "
        "    var z: int? := null; "
        "    var y: int? := 4; "
        "    Console.WriteLine(h.IdB(h.IdA(z)) ?? -1); "
        "    Console.WriteLine(GB<int?>(h.IdB(z)) ?? -1); "
        "    Console.WriteLine(h.Pick(y, z) ?? -1); "
        "    Console.WriteLine(h.Pick(z, h.IdB(y)) ?? -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n-1\n-1\n4\n");
}

// ─── Control: a NARROWED `int?` into a non-nullable `T := int` keeps the ──
// §7.4 unwrap (#1191), and into `T := int?` passes the present carrier.
BEZEL_TEST(generic_bare_tparam_nullable_arg_4899, narrowed_arg_controls) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method IdA(v: T): T => v; "
        "  public method IdB(v: T): T "
        "    return v; "
        "  end IdB; "
        "end Holder; "
        "public class C "
        "  public static method GB<U>(v: U): U "
        "    return v; "
        "  end GB; "
        "  public static method Main() "
        "    using var h := new Holder<int>(); "
        "    using var hn := new Holder<int?>(); "
        "    var x: int? := 9; "
        "    if x <> null then "
        "      Console.WriteLine(h.IdB(x)); "
        "      Console.WriteLine(h.IdA(x)); "
        "      Console.WriteLine(GB<int>(x)); "
        "      Console.WriteLine(hn.IdB(x) ?? -1); "
        "      Console.WriteLine(hn.IdA(x) ?? -1); "
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n9\n9\n9\n9\n");
}

// ─── Other nullable value payloads: long?, bool?, record? ─────────────────
// Control rows (measured: pass pre-fix too). The stray unwrap only fired when
// the payload's LLVM type equalled the bare-T i32 placeholder, so int? (and
// other i32-payload types) were the affected shapes; these pin that the
// restore leaves the wider/narrower/aggregate payloads correct.
BEZEL_TEST(generic_bare_tparam_nullable_arg_4899, long_bool_record_payloads) {
    JitResult r = runAotChecked(
        "module M; "
        "public record Pt(X: int); "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method IdB(v: T): T "
        "    return v; "
        "  end IdB; "
        "  public method IsNull(v: T): bool "
        "    return v = null; "
        "  end IsNull; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    using var hl := new Holder<long?>(); "
        "    using var hb := new Holder<bool?>(); "
        "    using var hp := new Holder<Pt?>(); "
        "    var l: long? := null; "
        "    var b: bool? := null; "
        "    var p: Pt? := null; "
        "    Console.WriteLine(hl.IdB(l) ?? -1); "
        "    Console.WriteLine(hl.IsNull(l)); "
        "    Console.WriteLine(hb.IsNull(b)); "
        "    Console.WriteLine(hp.IsNull(p)); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\nTrue\nTrue\nTrue\n");
}

// ─── Loop-amplified (50×): a Holder is allocated each iteration; ──────────
// alternates null/present so both carrier states are exercised and counted.
BEZEL_TEST(generic_bare_tparam_nullable_arg_4899, loop_amplified_50) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method IdA(v: T): T => v; "
        "  public method IdB(v: T): T "
        "    return v; "
        "  end IdB; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    var nulls := 0; "
        "    var sum := 0; "
        "    for var i in 0..<50 do "
        "      using var h := new Holder<int?>(); "
        "      var x: int? := null; "
        "      if i mod 2 = 1 then x := i; end; "
        "      if h.IdB(x) = null then nulls := nulls + 1; end; "
        "      if h.IdA(x) = null then nulls := nulls + 1; end; "
        "      sum := sum + (h.IdB(x) ?? 0); "
        "    end; "
        "    Console.WriteLine(nulls); "
        "    Console.WriteLine(sum); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "50\n625\n");
}
