// Regression tests for issue #4830 — §14.1 ("Generic declarations") / §7
// ("Nullable types") / §7.10 ("Generics and nullability") / §8.6 ("Boxing"),
// v0.307.
//
// THE DEFECT. A generic CLASS instantiated at a nullable VALUE type
// (`Holder<int?>`) lowers a bare (non-`?`-declared) type-parameter parameter
// `v: T` as the bare payload type (`i32`) instead of the tagged nullable-value
// carrier `{i8 hasValue, i32 value}` §7 requires. The class-level substitution
// itself (installed in `lowerCallDispatch`'s receiver-generic-args block) is
// correct — `genericSubst_["T"]` and `genericSubstSemaTy_["T"]` both resolve
// to `int?` with nullability preserved — but THREE downstream consumers of
// that correct substitution independently failed to recognize the "bare `T`
// resolves, under substitution, to a nullable value type" shape (only the
// EXPLICIT `T?`-in-source shape was handled, from fixes #567/#568):
//
//   (1) `emitGenericSpecialization` (LlvmBackend_Calls2.cpp): wraps a
//       BLOCK-bodied generic method's arguments into the tagged struct before
//       building/calling the specialized function. Missing case (C) left a
//       bare `i32`/`ptr null` argument unwrapped, so the specialized
//       function's declared param type (computed correctly elsewhere) and
//       the argument's actual LLVM type diverged:
//         Repro 1: `ToObj(v: T): Object?  return v; end ToObj;` called as
//           `hn.ToObj(6)` on `Holder<int?>` reached `boxForTarget` with a
//           correctly-resolved nullable-value `srcTy` but a bare `i32` VALUE
//           (never wrapped) — `boxValue`'s `CreateExtractValue` on a
//           non-struct value SIGSEGV'd (measured, exit 139).
//         Repro 2: `Id(v: T): T  return v; end Id;` called as `hn.Id(null)`
//           built a specialized function whose declared param/return type
//           (via `lowerTypeRef`'s `genericSubst_` lookup) was correctly
//           `{i8,i32}`, but the unwrapped `ptr null` argument mismatched it —
//           IR-verify failure.
//   (2) `emitInlineGenericMethod`'s EXPRESSION-body ("arrow-body", `=> v;`)
//       inline path binds params directly into the CALLER's own stack slots
//       (no specialized function is ever built for this shape) with the
//       SAME missing case, and its return-value handling (`boxForTarget` /
//       `wrapAsIEnumerableForTarget`) never covered a nullable-value target
//       at all. Measured: `Holder<bool?>.Id(null)` — an
//       AddressSanitizer stack-buffer-overflow, "WRITE of size 8" into a
//       2-byte `{i8,i1}` stack slot (a raw 8-byte pointer stored into a slot
//       the rest of the body's other reads treat as the 2-byte tagged
//       struct). The int?/long?/record? shapes did not surface this because
//       `Id`'s STATEMENT-form (`return v; end Id;`) reaches
//       `emitGenericSpecialization` instead — this file's rows deliberately
//       use the ARROW form for the matrix beyond the issue's own two repros
//       so this second site is exercised too.
//   (3) `coerceReturnToNullableValue` (LlvmBackend_Layout.cpp), the STATEMENT
//       `return` path's nullable-value return coercion, had the identical
//       gap for a bare, non-`?`-declared RETURN type resolving via
//       substitution to a nullable value type.
//
// THE FIX. A shared helper, `Emitter::wrapArgForNullableValueFormal`
// (declared in LlvmBackend_impl.hpp, defined in LlvmBackend_Calls2.cpp),
// adds the missing case (C) — `resolveTypeParamRef(formal)` on a bare,
// non-nullable-declared Named type-parameter ref, treating the resolved type
// as the wrap target when it differs from `formal` and is itself a nullable
// value type. `emitGenericSpecialization`'s existing three-case dispatch is
// replaced with a call to this helper; `emitInlineGenericMethod`'s
// expression-body parameter-binding loop now calls it too, and that same
// arm's return handling now calls `coerceReturnToNullableValue` (itself
// given the parallel case (C) fix) before the existing box/IEnumerable
// coercions.
//
// FAIL-WITHOUT (mechanically verified by the fixing lane): reverting
// LlvmBackend_impl.hpp / LlvmBackend_Calls2.cpp / LlvmBackend_Layout.cpp to
// origin/main and rebuilding reproduces, respectively: SIGSEGV (repro 1),
// IR-verify failure (repro 2), and an ASan stack-buffer-overflow
// (the bool? row) — all clean/correct with the fix.
//
// SCOPE NOTE on the boxing row (repro 1): `ToObj`'s `return v;` mints a
// heap box for the boxed `int` (§8.6). Per issue #2952's own documented scope
// boundary (see arg_position_box_release_2952_test.cpp), a box minted in
// RETURN position (as opposed to an argument position) is a SEPARATE,
// already-filed leak (the caller's `var o := hn.ToObj(6);` never releases
// it) — orthogonal to #4830's "wrong/unwrapped nullable-value shape" defect.
// That row therefore uses `runAotCheckedLenient` (ASan-instrumented,
// UAF/OOB-checked, leak detection OFF) rather than the leak-checked
// `runAotChecked`, so it still proves #4830's SIGSEGV is gone without
// asserting a fix to the unrelated, out-of-scope box leak. Every other row
// mints no such box and uses the full leak-checked `runAotChecked`.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

// ─── Repro 1 (#4830's own repro): SIGSEGV ─────────────────────────────────
// `ToObj(v: T): Object? return v; end ToObj;` on `Holder<int?>` called as
// `hn.ToObj(6)`. Pre-fix: SIGSEGV (exit 139) inside `boxValue`'s
// `CreateExtractValue` on the unwrapped bare `i32`. Post-fix: the boxed
// `Object` is non-null, so `o = null` is False.
BEZEL_TEST(generic_class_nullable_value_targ_4830, repro1_toobj_nonnull_no_sigsegv) {
    JitResult r = runAotCheckedLenient(
        "module Conf; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method ToObj(v: T): Object? "
        "    return v; "
        "  end ToObj; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    using var hn := new Holder<int?>(); "
        "    var o := hn.ToObj(6); "
        "    Console.WriteLine(o = null); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

// The null-payload sibling of repro 1: boxing a null `T` yields a null
// `Object?` (no box allocated — confirmed leak-clean under the full
// leak-checked harness), so `o = null` is True.
BEZEL_TEST(generic_class_nullable_value_targ_4830, toobj_null_returns_true) {
    JitResult r = runAotChecked(
        "module Conf; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method ToObj(v: T): Object? "
        "    return v; "
        "  end ToObj; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    using var hn := new Holder<int?>(); "
        "    var o := hn.ToObj(null); "
        "    Console.WriteLine(o = null); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// ─── Repro 2 (#4830's own repro): IR-verify failure ───────────────────────
// `Id(v: T): T return v; end Id;` (STATEMENT body — reaches
// `emitGenericSpecialization`) on `Holder<int?>` called as `hn.Id(null)`.
// Pre-fix: IR-verify failure (`ret ptr` against `{i8,i32}`). Post-fix: null
// propagates through `??`.
BEZEL_TEST(generic_class_nullable_value_targ_4830, repro2_id_null_no_ir_fail) {
    JitResult r = runAotChecked(
        "module Conf; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method Id(v: T): T "
        "    return v; "
        "  end Id; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    using var hn := new Holder<int?>(); "
        "    var b: int? := hn.Id(null); "
        "    Console.WriteLine(b ?? -1); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// Non-null sibling of repro 2, same STATEMENT-body `Id`: a present value
// round-trips through the specialized function's tagged-struct param/return.
BEZEL_TEST(generic_class_nullable_value_targ_4830, id_nonnull_int_present_value) {
    JitResult r = runAotChecked(
        "module Conf; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method Id(v: T): T "
        "    return v; "
        "  end Id; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    using var hn := new Holder<int?>(); "
        "    var b: int? := hn.Id(6); "
        "    Console.WriteLine(b ?? -1); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

// ─── Field store/load at a bare-T field on Holder<int?> ───────────────────
// `f: T` (a plain instance field, not a parameter/return) — a THIRD site
// (buildRecordValue's default-init loop) had the analogous gap for RECORD
// body fields (#3944); this pins the CLASS field-assignment/read path is
// unaffected (it already resolved T's substituted LLVM type correctly, since
// the field's storage slot type comes straight from `lowerTypeRef` under the
// active class substitution, not from an argument/return value).
BEZEL_TEST(generic_class_nullable_value_targ_4830, field_store_load_present_and_null) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public f: T; "
        "  public constructor() end constructor; "
        "  public method Set(v: T) f := v; end Set; "
        "  public method Get(): T => f; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    using var h := new Holder<int?>(); "
        "    h.Set(7); "
        "    Console.WriteLine(h.Get() ?? -1); "
        "    h.Set(null); "
        "    Console.WriteLine(h.Get() ?? -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n-1\n");
}

// ─── Holder<long?> — a WIDER (8-byte) nullable-value payload ──────────────
// ARROW-body `Id` (`=> v;`) — reaches `emitInlineGenericMethod`'s
// expression-body path, the second fixed site.
BEZEL_TEST(generic_class_nullable_value_targ_4830, holder_long_nullable_present_and_null) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method Id(v: T): T => v; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    using var h := new Holder<long?>(); "
        "    var a: long? := h.Id(9000000000L); "
        "    Console.WriteLine(a ?? -1); "
        "    var b: long? := h.Id(null); "
        "    Console.WriteLine(b ?? -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "9000000000\n-1\n");
}

// ─── Holder<bool?> — the MEASURED ASan stack-buffer-overflow repro ────────
// `bool`'s tagged struct `{i8, i1}` is exactly 2 bytes — the narrowest
// nullable-value payload — which is what turned the inline path's unwrapped
// `ptr null` argument into an 8-byte write onto a 2-byte stack object.
// Pre-fix (measured): AddressSanitizer stack-buffer-overflow, "WRITE of size
// 8" into a `[32, 34)`-sized frame object, on the null call specifically
// (the non-null `Id(true)` call was already ASan-clean before this fix).
BEZEL_TEST(generic_class_nullable_value_targ_4830, holder_bool_nullable_present_and_null) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method Id(v: T): T => v; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    using var h := new Holder<bool?>(); "
        "    var a: bool? := h.Id(true); "
        "    Console.WriteLine(a ?? false); "
        "    var b: bool? := h.Id(null); "
        "    Console.WriteLine(b = null); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\n");
}

// Loop-amplified (50 iterations) sibling of the bool? stack-overflow repro:
// a one-shot run can leave a corrupted stack byte unread by anything that
// would notice, and ASan's redzone check depends on the exact frame layout
// at the moment of the overflowing write — 50 fresh calls (fresh allocas
// each iteration, per §28.3 scope-exit-and-reenter) make a residual
// off-by-N-bytes defect overwhelmingly likely to be caught if it still
// exists. Asserts BOTH the per-iteration correctness (every iteration
// observes `b = null`) and the total count, so a defect that silently
// skips iterations cannot pass by accident.
BEZEL_TEST(generic_class_nullable_value_targ_4830, holder_bool_nullable_null_loop_amplified_50) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method Id(v: T): T => v; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    var nullCount := 0; "
        "    for var i in 0..<50 do "
        "      using var h := new Holder<bool?>(); "
        "      var b: bool? := h.Id(null); "
        "      if b = null then nullCount := nullCount + 1; end; "
        "    end; "
        "    Console.WriteLine(nullCount); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "50\n");
}

// ─── Holder<Pt?> — a RECORD (aggregate-payload) nullable value type ───────
// The payload slot itself is a multi-field aggregate (`{i32 x}`), not a
// scalar — confirms the wrap/unwrap logic is not scalar-only.
BEZEL_TEST(generic_class_nullable_value_targ_4830, holder_record_nullable_present_and_null) {
    JitResult r = runAotChecked(
        "module M; "
        "public record Pt(x: int); "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method Id(v: T): T => v; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    using var h := new Holder<Pt?>(); "
        "    var a: Pt? := h.Id(new Pt(11)); "
        "    Console.WriteLine((a!).x); "
        "    var b: Pt? := h.Id(null); "
        "    Console.WriteLine(b = null); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nTrue\n");
}

// ─── Control: Holder<int> (NON-nullable instantiation) is unaffected ──────
// The bare-T-resolves-to-nullable-value shape does not apply when the class
// is instantiated at a non-nullable value type; `resolveTypeParamRef` still
// resolves `T` but `isNullableValueTy` on the result is false, so
// `wrapArgForNullableValueFormal` is a no-op (returns the argument
// unchanged) and the plain `i32` path is exercised exactly as before this
// fix.
BEZEL_TEST(generic_class_nullable_value_targ_4830, holder_int_nonnullable_control) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Holder<T> "
        "  public constructor() end constructor; "
        "  public method Id(v: T): T => v; "
        "end Holder; "
        "public class C "
        "  public static method Main() "
        "    using var h := new Holder<int>(); "
        "    Console.WriteLine(h.Id(6)); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}
