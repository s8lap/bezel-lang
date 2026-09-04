// fix #3772 (§27.4 / §14.1, v0.287): a constructor-argument lambda literal
// whose unannotated parameter type is a GENERIC CLASS's own type parameter,
// resolved only via the explicit type argument at the `new` call site,
// compiled cleanly and silently computed the WRONG runtime value (printed
// `0`) when the closure escaped the constructor and was invoked from
// elsewhere.
//
// Spec §27.4 "Parameter type inference", verbatim: "When the lambda's
// target type is known from context, parameter type annotations may be
// omitted". The class-generic-then-instantiated `new` expression IS a
// target-type context — the matched constructor parameter `(T) -> int`
// on `Box<T>` instantiated as `Box<R>` is `(R) -> int`, and §14.1
// binds the class's generic args (T → R) at the call site. Pre-fix
// the NewExpr visit loop in ExprTypecheck_Exprs.cpp's `visitChildren`
// had no `pendingLambdaParamTypes_`/`pendingLambdaParamType_` threading
// at all (unlike the CallExpr argument loop, whose fix-#829 / fix-#1140
// machinery applies the same `classSubst` from the receiver's bound
// generic args); the lambda's parameter `x` therefore stayed untyped
// against the bare class type-parameter `T`, and codegen computed `0`
// from the unboxed `R` reference once the closure was invoked later
// through a different method. Non-generic class receivers and the
// same-generic-class-invoked-synchronously-in-ctor cases happened to
// produce the right value via a different specialization path; this
// specific combination fell through to the silent-wrong direction.
//
// The fix threads the class's bound generic args through
// `pendingLambdaParamTypes_` / `pendingLambdaTargetReturn_` for each
// constructor argument whose value is a LambdaExpr (the same shape as
// the CallExpr arm's `classSubst` block, ~lines 7391-7497), so the
// lambda's parameters are inferred to the concrete substituted type.
//
// ACCEPT CONTROLS — every neighbouring constructor-argument + lambda
// shape that must keep compiling and producing the right value is
// pinned: a non-generic receiving class (whose lambda param is the
// concrete type already), a generic class whose ctor-arg lambda is
// invoked SYNCHRONOUSLY inside the ctor body (fix #3756's own
// `generic_probe3` measurement), and a generic class with an
// ANNOTATED ctor-arg lambda param (where the explicit annotation
// already names the type and the threading is a no-op).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The issue repro, verbatim (modulo module boilerplate). Generic class
// `Box<T>` with a ctor-arg lambda bound to `(T) -> int`; instantiated
// as `Box<R>` where `R.V() == 5`; the closure escapes the ctor and is
// invoked from `Box<T>::Run`. Pre-fix prints `0`; post-fix prints `5`.
const char* kIssueRepro =
    "module M; import System; "
    "class R "
    "  private v: int := 5; "
    "  public method V(): int return this.v; end V; "
    "end R; "
    "class Box<T> "
    "  private scoped f: (T) -> int; "
    "  public constructor(scoped g: (T) -> int) f := g; end constructor; "
    "  public method Run(v: T): int return f(v); end Run; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    var b := new Box<R>(lambda(x) => x.V()); "
    "    Console.WriteLine(b.Run(new R()).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// Non-generic receiving class — `Foo` instead of `Box<T>`, the lambda
// param type is the concrete `R`, no class-level substitution is
// required. Prints `5` already pre-fix; the post-fix threading is a
// no-op for this case (no classSubst to apply) and must keep the value
// correct (no regression).
const char* kNonGenericReceiver =
    "module M; import System; "
    "class R "
    "  private v: int := 5; "
    "  public method V(): int return this.v; end V; "
    "end R; "
    "class Foo "
    "  private scoped f: (R) -> int; "
    "  public constructor(scoped g: (R) -> int) f := g; end constructor; "
    "  public method Run(v: R): int return f(v); end Run; "
    "end Foo; "
    "class C "
    "  public static method Main(): int "
    "    var b := new Foo(lambda(x) => x.V()); "
    "    Console.WriteLine(b.Run(new R()).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// Generic class whose ctor-arg lambda's parameter type does NOT use
// the class's own type parameter (`(int) -> int` rather than
// `(T) -> int`). This is the synchronous-invocation analogue of the
// issue's repro WITHOUT the bug trigger — no class-level substitution
// is required for the lambda's parameter, so it prints `14` already
// pre-fix and the post-fix `classSubst` block is a no-op here (the
// classSubst map is empty, no TypeRef substitution runs). Pinned by
// value so any future change that introduces an over-substitution
// regression on this shape would go RED.
const char* kSynchronousCtorInvoke =
    "module M; import System; "
    "class R "
    "  private v: int := 1; "
    "end R; "
    "class Box<T> "
    "  private scoped f: (int) -> int; "
    "  public constructor(scoped g: (int) -> int) "
    "    Console.WriteLine(g(7).ToString()); "
    "    f := g; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    using var b := new Box<R>(lambda(x) => x * 2); "
    "    return 0; "
    "  end Main; "
    "end C;";

// Generic class with an ANNOTATED ctor-arg lambda param — the
// parameter's type is spelled out (`x: T`) so the lambda visitor
// already binds `x` to `T` from the explicit annotation and never
// relies on the threading. The fix's classSubst block must keep this
// working byte-for-byte (the annotation wins over the inferred type
// anyway, so the threading is a no-op).
const char* kAnnotatedParam =
    "module M; import System; "
    "class R "
    "  private v: int := 5; "
    "  public method V(): int return this.v; end V; "
    "end R; "
    "class Box<T> "
    "  private scoped f: (T) -> int; "
    "  public constructor(scoped g: (T) -> int) f := g; end constructor; "
    "  public method Run(v: T): int return f(v); end Run; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    var b := new Box<R>(lambda(x: R) => x.V()); "
    "    Console.WriteLine(b.Run(new R()).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// Row 1: the issue repro. Pre-fix prints `0`; post-fix prints `5`.
// The test program's `Console.WriteLine` is on the captured stdout;
// exitCode is `0` either way (the miscompute does not crash), so the
// pin is on `stdoutText`.
BEZEL_TEST(sema_generic_class_ctor_arg_lambda_3772,
           issue_repro_escaped_closure_prints_5_spec_27_4) {
    JitResult r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// Row 2: control — non-generic receiving class. Already worked pre-fix;
// post-fix must not regress it.
BEZEL_TEST(sema_generic_class_ctor_arg_lambda_3772,
           non_generic_receiver_unaffected_spec_27_4) {
    JitResult r = runAot(kNonGenericReceiver);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// Row 3: control — generic class with ctor-arg lambda invoked
// SYNCHRONOUSLY inside the ctor body (the #3756 `generic_probe3`
// shape). Already worked pre-fix; post-fix must not regress it.
BEZEL_TEST(sema_generic_class_ctor_arg_lambda_3772,
           synchronous_ctor_invoke_unaffected_spec_27_4) {
    JitResult r = runAot(kSynchronousCtorInvoke);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "14\n");
}

// Row 4: control — generic class with ANNOTATED ctor-arg lambda
// param. The annotation names the type so the threading is a no-op;
// post-fix must keep printing `5`.
BEZEL_TEST(sema_generic_class_ctor_arg_lambda_3772,
           annotated_ctor_lambda_param_unaffected_spec_27_4) {
    JitResult r = runAot(kAnnotatedParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}