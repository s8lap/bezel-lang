// Regression tests for issue #4973 — silent wrong code: direct T-typed property
// get/set on non-generic `E: B<string>` uses erased accessor (§14.1 / §23 /
// §2.8.3 rule 4, v0.307).
//
// THE DEFECT (silent wrong code, measured). The block-form property getter for
// a T-typed property inherited from a generic base `B<T>` and called directly on
// a non-generic receiver `e: E: B<string>` named the function as the ERASED
// template `bz$prop$<name>$get` (signature `i32(ptr)`), because
// `ensurePropertyBlockGetter` builds the name+signature from
// `lowerType(pd->type.get())` and — when no `genericSubst_` is active — `T`
// falls back to `i32()`. The receiver is a ptr to the COMPOSED layout (8 bytes
// for string), but the call type-mismatched it. The setter twin
// (`lowerPropertySetValue` in LlvmBackend_Assign.cpp) had the parallel defect:
// the inlined setter body lowered `_v := value` against the erased B<T> struct
// (T=i32) while `value` was bound to a ptr slot, so the write succeeded via the
// opaque-ptr ABI but the upper 4 bytes of `_v` were silently clobbered. The
// indexer twin (`ensureIndexerBlockGetter` in LlvmBackend_Assign.cpp) had the
// same erased-accessor defect as the block-form getter.
//
// Measured repros:
//   - p3c (block-form getter + §22.12 static): prints `5835200\n0\n` pre-fix
//     instead of `y\n102\n`.
//   - p7_direct (setter + getter + indexer, on `E: B<string>`): prints
//     `x x <empty> <empty>` pre-fix instead of `x x y y`. The two `<empty>`
//     lines are the i32-typed getter reads returning the first 4 bytes of the
//     actual `_v` field, with the upper 4 bytes truncated to zero by
//     `lowerType(T) -> i32()`.
//
// THE FIX. The block-form getter call site at LlvmBackend_Exprs.cpp:6208 (CG33b
// arm of `lowerMemberExpr`) now mirrors the expression-bodied getter path at
// ~5989: when the receiver is a non-generic class whose declared class is not
// the property's declaring class, and the declaring class has `genericParams`,
// `computeAncestorGenericSubst` walks the base chain and installs the resulting
// substitution into `genericSubst_` / `genericSubstSemaTy_` BEFORE
// `ensurePropertyBlockGetter` runs. Save+restore keeps the subst from leaking
// into the enclosing expression. The SETTER twin at LlvmBackend_Assign.cpp:6859
// has the parallel install-subst-before-inline fix; the INDEXER twin at
// LlvmBackend_Assign.cpp:6089 has the parallel
// install-subst-before-`ensureIndexerBlockGetter` fix.
//
// SCOPE OF THIS FIX. The single-module DIRECT CALL cases (p3c + p7_direct).
// The cross-module import path goes through a separate site (CG33b's
// `importedPropGetterDirectFns_[ps]` route at LlvmBackend_Exprs.cpp:6246), not
// exercised here. The static-method and static-field inherited-from-generic-
// base twins are also separate paths.
//
// SUITE LAYOUT. Two suites, by defect arm:
//   - `codegen_4973_getter` — block-form getter defect (p3c) + the same-class
//     control that pins the fix doesn't break the already-correct path (this
//     file's first commit, which lands on the sibling `r21c-4973-getter`
//     branch).
//   - `direct_property_call_erased_accessor_4973` — direct setter+getter+indexer
//     defect (p7_direct) + two control rows. The control row
//     `control_same_class_no_subst_needed` is renamed to `_twin` here to
//     disambiguate from the getter branch's same-named control: the body is
//     functionally identical, but this version lives under this suite because
//     it also runs `runAotChecked` (the getter control uses only `runAot`).

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"          // BEZEL_TEST / BEZEL_EXPECT_*

using bezel::test::runAot;

// ===========================================================================
// P3C: block-form getter on `e: E: B<string>` reads the i32-shaped first 4
// bytes of `_v: string` (a pointer), pre-fix printing `5835200` instead of
// `y`. The §22.12 static `B<string>._c` is read on the next line — it must
// stay 102 (the second-class-slot count; not perturbed by the property
// defect).
//
// Post-fix: `y\n102\n`.
// ===========================================================================
BEZEL_TEST(codegen_4973_getter, p3c_block_getter_with_static) {
    const char* src = R"(
        module M;
        public class B<T>
          public static _c: int := 102;
          public _v: T;
          public constructor(v: T) _v := v; end constructor;
          public property Val: T
            get => do return _v; end;
            set => do _v := value; end;
          end Val;
        end B;
        public class E: B<string>
          public constructor() base("y"); end constructor;
        end E;
        public class Program
          public static method Main(): int
            using var e := new E();
            e.Val := "y";
            Console.WriteLine(e.Val);
            Console.WriteLine(B<string>._c);
            return 0;
          end Main;
        end Program;
    )";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "y\n102\n");
}

// ===========================================================================
// Control: a property call on the GENERIC BASE directly (`b.Val` where
// `b: B<string>`) does NOT need the fix — the subst is installed by the
// call-site machinery at the generic-parameter-binding walk. Pre-fix this
// would also work, because the receiver class IS the property's declaring
// class. The test pins that the fix doesn't break the already-correct path.
// ===========================================================================
BEZEL_TEST(codegen_4973_getter, control_same_class_no_subst_needed) {
    const char* src = R"(
        module M;
        public class B<T>
          private _v: T;
          public constructor(v: T) _v := v; end constructor;
          public property Val: T
            get => _v;
            set => do _v := value; end;
          end Val;
        end B;
        public class Program
          public static method Main(): int
            using var b := new B<string>("x");
            Console.WriteLine(b.Val);
            b.Val := "y";
            Console.WriteLine(b.Val);
            return 0;
          end Main;
        end Program;
    )";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "x\ny\n");
}

// ===========================================================================
// P7: direct T-typed property setter on non-generic `E: B<string>`.
// Pre-fix, the second `e.Ro` after `e.Val := "y"` reads the first 4 bytes
// of `_v` (now "y" = 8 bytes) and zero-truncates the upper 4 — the
// `lowerType(T) -> i32()` erased accessor signature's wrong read. Post-fix
// the getter call sites install the ancestor-generic subst first, so the
// emitted accessor is `ptr(ptr)` and reads all 8 bytes.
// ===========================================================================
BEZEL_TEST(direct_property_call_erased_accessor_4973,
           p7_direct_setter_then_getter) {
    // Verbatim from /var/tmp/bezel-scratch/r20/rv-l31/p7_direct.bzl
    // (rv-L31's measurement of the silent-wrong-code defect).
    const char* src =
        "module M; "
        "public class B<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property Val: T "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end Val; "
        "  public property Ro: T => _v; "
        "end B; "
        "public class E: B<string> "
        "  public constructor() base(\"x\"); end constructor; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    Console.WriteLine(e.Ro); "
        "    Console.WriteLine(e.Val); "
        "    e.Val := \"y\"; "
        "    Console.WriteLine(e.Val); "
        "    Console.WriteLine(e.Ro); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto plain = bezel::test::runAot(src);
    auto checked = bezel::test::runAotChecked(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, "x\nx\ny\ny\n");
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, "x\nx\ny\ny\n");
}

// ===========================================================================
// Control: same as the getter branch's `control_same_class_no_subst_needed`,
// but under this suite and run with `runAotChecked` too. The body is
// functionally identical (the test pins that the fix doesn't break the
// already-correct path for the direct setter+getter on the generic base).
// Named `_twin` to avoid the gtest suite.name collision.
// ===========================================================================
BEZEL_TEST(direct_property_call_erased_accessor_4973,
           control_same_class_no_subst_needed_twin) {
    const char* src =
        "module M; "
        "public class B<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property Val: T "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end Val; "
        "end B; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new B<string>(\"x\"); "
        "    Console.WriteLine(b.Val); "
        "    b.Val := \"y\"; "
        "    Console.WriteLine(b.Val); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto plain = bezel::test::runAot(src);
    auto checked = bezel::test::runAotChecked(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, "x\ny\n");
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, "x\ny\n");
}

// ===========================================================================
// Control: when the inherited property's declared type is `int` (NOT a
// generic type-parameter), the layout-move predicate (the #2896 gate the
// expression-bodied path also uses as a fast-path) is false but the subst
// gate at ~5989 ALSO evaluates false (`ps->parent->genericParams.empty()`),
// so the install-subst block is skipped entirely. The test pins that
// the new fix does not over-install subst for non-generic property types
// and that the call still works through `ensurePropertyBlockGetter`.
// ===========================================================================
BEZEL_TEST(direct_property_call_erased_accessor_4973,
           control_int_field_no_layout_move) {
    const char* src =
        "module M; "
        "public class B<T> "
        "  public _v: int; "
        "  public constructor() _v := 7; end constructor; "
        "  public property Val: int "
        "    get => do return _v; end; "
        "    set => do _v := value; end; "
        "  end Val; "
        "end B; "
        "public class E: B<string> "
        "  public constructor() base(); end constructor; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    e.Val := 100; "
        "    Console.WriteLine(e.Val); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto plain = bezel::test::runAot(src);
    auto checked = bezel::test::runAotChecked(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, "100\n");
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, "100\n");
}

// ===========================================================================
// P7-Direct setter_loop_200_iter (#4983, §14.1 / §23 / §2.8.3 rule 4,
// v0.307): the SAME-CLASS twin of `p7_direct_setter_then_getter` above,
// driven through a 200-iteration loop so the wrong-offset write can no
// longer happen to land on benign bytes — every iteration's RHS
// allocates a FRESH string and the SETTER body writes it into the
// backing field; pre-fix the write lands at the i32-shaped offset 12
// of the erased `%class.B` template layout, leaving the substituted
// offset 16 at whatever prior bytes were there (stale or padding).
// The final `Console.WriteLine(b.Val)` therefore reads garbage or empty
// — pre-fix the program prints `\n\n` (3 bytes 78 0a 0a from
// `x` + 2 newlines + the FINAL getter reading empty), post-fix it prints
// `v199\n`. Runs under `runAot` only — the ASan/LSan path trips a
// separate §28.9 leak in the constructor's `_v := v` body (the
// constructor uses the erased T=i32 layout too, writing only 4 bytes
// to offset 12 instead of 8 bytes to offset 16 — this is a separate
// issue from the setter twin #4983 fixes), and that leak is outside the
// scope of #4983. The 200-iteration amplification IS the regression
// pin: a 1-shot write is not enough to catch the wrong-offset defect
// on its own because the SETTER always correctly stores `value` SOMEWHERE
// — the question is whether the READ sees that store. The plain
// `runAot` printout (`x\ny\n` for the 1-shot rows, `v199\n` here)
// discriminates the two layouts.
// ===========================================================================
BEZEL_TEST(direct_property_call_erased_accessor_4973,
           setter_loop_200_iter_4983) {
    const char* src =
        "module M; "
        "public class B<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property Val: T "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end Val; "
        "end B; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new B<string>(\"x\"); "
        "    for var i in 0..<200 do "
        "      b.Val := \"v\" + i.ToString(); "
        "    end; "
        "    Console.WriteLine(b.Val); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto plain = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, "v199\n");
}
