// Regression tests for issue #4973 — silent wrong code: direct T-typed
// BLOCK-FORM PROPERTY GETTER on non-generic `E: B<string>` uses erased
// accessor (§14.1 / §23 / §2.8.3 rule 4, v0.307; #3346/#3348 sibling).
//
// THE DEFECT (silent wrong code, measured). The block-form property getter
// for a T-typed property inherited from a generic base `B<T>` and called
// directly on a non-generic receiver `e: E: B<string>` named the function
// as the ERASED template `bz$prop$<name>$get` (signature `i32(ptr)`), because
// `ensurePropertyBlockGetter` builds the name+signature from
// `lowerType(pd->type.get())` and — when no `genericSubst_` is active — `T`
// falls back to `i32()`. The receiver is a ptr to the COMPOSED layout (8 bytes
// for string), but the call type-mismatched it. The i32 return is the first 4
// bytes of the actual `_v: T` field — empty for a string-typed T.
//
// Measured repro from rv-L31: p3c.bzl prints `5835200\n0\n` (a pointer
// truncated to int, then the wrong §22.12 static slot). The fix must restore
// `y\n102\n`.
//
// THE FIX. The block-form getter call site at LlvmBackend_Exprs.cpp:6208
// (CG33b arm of `lowerMemberExpr`) now mirrors the expression-bodied getter
// path at ~5989: when the receiver is a non-generic class whose declared class
// is not the property's declaring class, and the declaring class has
// `genericParams`, `computeAncestorGenericSubst` walks the base chain and
// installs the resulting substitution into `genericSubst_` /
// `genericSubstSemaTy_` BEFORE `ensurePropertyBlockGetter` runs. Save+restore
// keeps the subst from leaking into the enclosing expression.
//
// SCOPE OF THIS LANE. The block-form getter + §22.12 static case the issue
// body calls out as p3c. The SETTER twin (`lowerPropertySetValue` in
// LlvmBackend_Assign.cpp) and the EXPRESSION-BODIED GETTER twin (already
// fixed by #3346/#3348) are the other arms — the SETTER is fixed on the
// sibling `4973-twins` lane. The INDEXER twin (`ensureIndexerBlockGetter`)
// is also a separate audit. The cross-module IMPORTED property path goes
// through a separate site (CG33b's `importedPropGetterDirectFns_[ps]` route
// at LlvmBackend_Exprs.cpp:6246), out of scope here.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

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
