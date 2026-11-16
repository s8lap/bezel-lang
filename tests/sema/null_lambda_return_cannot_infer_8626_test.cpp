// Spec-derived regression test for issue #8626
// (spec §14.5 phase 1 + phase 2 + phase 3, v0.357).
//
// THE DEFECT. A lambda literal whose body is a BARE `null` was admitted
// as the argument of a function-typed parameter whose return slot names
// a type parameter, and that type parameter stayed UNBOUND — the call
// compiled clean and the program computed a wrong answer.
//
// MEASURED on the pre-fix compiler (r30-c1 tip 27ad51b93):
// `Seq.M(xs, lambda(v) => null)` against `M<T, K>(xs: Array<T>, key: (T)
// -> K): T` compiled, ran, and printed the FIRST element. The lambda's
// return bound `K` to the null literal's `Object?` — the uncontexted-null
// sentinel, a binding that carries no type information — so `k` was a
// null inside the callee, `k.CompareTo(bk)` never selected anything, and
// the method returned `xs[0]`. The `where K: IComparable<K>` constraint
// was not enforced either: a nullable Object satisfies no interface
// constraint, and the constraint check has no verdict for it.
//
// WHY IT MUST BE REJECTED. §14.5 phase 1: "a bare `null` literal
// argument is not self-determined and contributes no binding". §14.5
// phase 2 binds "the lambda's inferred return type" — and a body that is
// a bare `null` has no self-determined return type. So the type
// parameter such a position was to bind stays unbound, and §14.5 phase 3
// makes that "a compile error naming the parameter".
//
// The fix is in `typeCall` (ExprTypecheck_Calls.cpp): a bare-null lambda
// body contributes no return binding (the inference), and a type
// parameter that such a position was to bind and that nothing else bound
// is reported as the phase-3 "cannot infer" error.
//
// FIX-FORWARD (review r42-rev-m39-8625, Finding A). Phase 3 first binds a
// still-unbound parameter "from the call's target type" — the initializer
// of a typed declaration, a `return` against a declared return type, ... —
// and only a parameter that "remains unbound — it appears in no
// self-determined argument position and no target context supplies it —
// is a compile error". The first version reported the error before the
// target had its say, so `var r: Object? := Q.F(xs, lambda(v) => null)`
// (K := Object from the target) was rejected. The target rows below pin
// the completion; the no-target row keeps the rejection.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The issue's own repro: the lambda's parameter is un-annotated, so the
// lambda is typed by phase 2 (§14.5) — the null body is its only return.
const char kUnannotatedLambda[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, lambda(v) => null).Name);
  end Main;
end Program;
)";

// The same shape with the lambda's parameter ANNOTATED: the lambda's
// parameter types are self-determined (§14.5 phase 1) but its RETURN is
// still the bare `null`.
const char kAnnotatedLambda[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, lambda(v: Box) => null).Name);
  end Main;
end Program;
)";

// True iff the rejection is the §14.5 phase-3 "cannot infer" diagnostic
// naming the type parameter.
bool hasCannotInferDiag(const JitResult& r) {
    return diagnosticMessageContains(r, "cannot infer the type argument `K`") &&
           diagnosticMessageContains(r, "bare `null`");
}


// ---------------------------------------------------------------------------
// §14.5 phase 3 target-typed completion (fix-forward, Finding A).
// ---------------------------------------------------------------------------

// `F<T, K>(xs: Array<T>, key: (T) -> K?): K?` — the lambda's bare `null`
// body leaves `K` unbound after phases 1 and 2; only a target can bind it.
const char kNullableKeyHeader[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Q
  public static method F<T, K>(xs: Array<T>, key: (T) -> K?): K?
    return key(xs[0]);
  end F;
)";

std::string nullableKeyProgram(const char* extraMembers, const char* mainBody) {
    return std::string(kNullableKeyHeader) + extraMembers +
           "end Q;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    var xs: Array<Box> := [new Box(\"ann\", 30), new Box(\"dan\", 35), "
           "new Box(\"bob\", 25), new Box(\"cho\", 40)];\n" +
           mainBody +
           "  end Main;\n"
           "end Program;\n";
}

// Review probe e08: a typed local `Object?` binds `K` := Object (`K?`
// against `Object?` binds through the nullable wrapping), and `null` is
// admitted at the substituted `(Box) -> Object?` slot. The pre-fix compiler
// ran it (`True`); the first #8626 version rejected it.
const std::string kObjectTargetLocal = nullableKeyProgram(
    "",
    "    var r: Object? := Q.F(xs, lambda(v) => null);\n"
    "    Console.WriteLine(r = null);\n");

// Review probe f02: an `int?` typed local and an `int?` declared return
// type each bind `K` := int.
const std::string kIntTargetLocalAndReturn = nullableKeyProgram(
    "  public static method G(xs: Array<Box>): int?\n"
    "    return F(xs, lambda(v) => null);\n"
    "  end G;\n",
    "    var r: int? := Q.F(xs, lambda(v) => null);\n"
    "    Console.WriteLine(r = null);\n"
    "    Console.WriteLine(Q.G(xs) = null);\n");

// Review probe b03/d04: a `string?` typed local binds `K` := string. The
// pre-fix compiler rejected it too ("`Object` converts implicitly to no
// other type"), K having been bound to the null literal's `Object`.
const std::string kStringTargetLocal = nullableKeyProgram(
    "",
    "    var r: string? := Q.F(xs, lambda(v) => null);\n"
    "    Console.WriteLine(r ?? \"none\");\n");

// Review probe d05: a `return` against a declared `string?` return type.
const std::string kStringTargetReturn = nullableKeyProgram(
    "  public static method G(xs: Array<Box>): string?\n"
    "    return F(xs, lambda(v) => null);\n"
    "  end G;\n",
    "    Console.WriteLine(Q.G(xs) ?? \"none\");\n");

// Review probe b04: no target at all (`var` without a written type) — `K`
// remains unbound and the call is the phase-3 error.
const std::string kNoTarget = nullableKeyProgram(
    "",
    "    var r := Q.F(xs, lambda(v) => null);\n"
    "    Console.WriteLine(\"done\");\n");

// Review probe f04: a BARE `K` return slot, `(T) -> K`, against an
// `Object?` target — `K` := Object? (a bare parameter binds to the target
// type itself). Only admission is asserted: the value the program computes
// is wrong on the pre-fix compiler as well (codegen's #885/#886 guard in
// unifyMethodReturnTypeArg declines to bind a type parameter to the
// `Object` primitive, so the callee is emitted at the i32 placeholder) —
// filed separately, not this fix.
const char kBareKeyObjectTarget[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Q
  public static method F<T, K>(xs: Array<T>, key: (T) -> K): K
    return key(xs[0]);
  end F;
end Q;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("dan", 35), new Box("bob", 25), new Box("cho", 40)];
    var r: Object? := Q.F(xs, lambda(v) => null);
    Console.WriteLine("done");
  end Main;
end Program;
)";

// Review probe b07: a NON-nullable `string` target binds `K` := string, and
// then the bare `null` body is not admitted at the substituted
// `(Box) -> string` slot (§5.1: `null` converts only to a nullable type).
const char kBareKeyNonNullableTarget[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Q
  public static method F<T, K>(xs: Array<T>, key: (T) -> K): K
    return key(xs[0]);
  end F;
end Q;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("dan", 35), new Box("bob", 25), new Box("cho", 40)];
    var r: string := Q.F(xs, lambda(v) => null);
    Console.WriteLine(r);
  end Main;
end Program;
)";

bool compiledAndPrinted(const JitResult& r, const char* expected) {
    return !r.diagnostics.hasErrors() && r.ran && r.exitCode == 0 &&
           r.stdoutText == expected;
}
}  // namespace

// ===========================================================================
// REJECT tests — these FAIL without the fix, PASS with it
// ===========================================================================

// The issue's own repro. Before the fix the call compiled, ran, and
// printed the first element (`ann`) — the type parameter `K` was bound to
// the null literal's `Object?` and the method never selected anything.
// With the fix the bare-`null` body contributes no binding, `K` remains
// unbound, and §14.5 phase 3 rejects the call naming `K`.
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           unannotated_lambda_returning_null_cannot_infer_spec_14_5_phase_3) {
    auto r = runAot(kUnannotatedLambda);
    BEZEL_EXPECT_TRUE(hasCannotInferDiag(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// The annotated spelling of the same shape. The lambda's parameter type
// is self-determined (§14.5 phase 1) but its return is a bare `null`,
// which binds nothing — so `K` is still unbound and the call is still
// the phase-3 "cannot infer" error.
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           annotated_lambda_returning_null_cannot_infer_spec_14_5_phase_3) {
    auto r = runAot(kAnnotatedLambda);
    BEZEL_EXPECT_TRUE(hasCannotInferDiag(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ===========================================================================
// §14.5 phase 3 target-typed completion — ADMIT rows. Each FAILS on the
// first #8626 version (3438fedb1: "cannot infer the type argument `K`")
// and PASSES with the fix-forward.
// ===========================================================================

// §14.5 phase 3: "the initializer of a typed declaration" is a target
// context; `K?` against `Object?` binds K := Object.
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           object_typed_local_target_binds_k_spec_14_5_phase_3) {
    auto r = runAot(kObjectTargetLocal);
    BEZEL_EXPECT_TRUE(compiledAndPrinted(r, "True\n"));
}

// §14.5 phase 3: a typed local and "a `return` statement against a
// declared return type" each supply `K` := int.
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           int_typed_local_and_return_target_bind_k_spec_14_5_phase_3) {
    auto r = runAot(kIntTargetLocalAndReturn);
    BEZEL_EXPECT_TRUE(compiledAndPrinted(r, "True\nTrue\n"));
}

// §14.5 phase 3 at a bare `K` return slot: K := Object? from the target.
// Admission only (see kBareKeyObjectTarget's comment).
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           bare_k_slot_object_target_admitted_spec_14_5_phase_3) {
    auto r = runAot(kBareKeyObjectTarget);
    BEZEL_EXPECT_TRUE(compiledAndPrinted(r, "done\n"));
}

// §14.5 phase 3, `string?` typed local: K := string. Also red on r30-c1,
// which bound K to the null literal's `Object` and then rejected the
// initializer's conversion.
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           string_typed_local_target_binds_k_spec_14_5_phase_3) {
    auto r = runAot(kStringTargetLocal);
    BEZEL_EXPECT_TRUE(compiledAndPrinted(r, "none\n"));
}

// §14.5 phase 3, `return` against a declared `string?`: K := string. Also
// red on r30-c1.
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           string_return_target_binds_k_spec_14_5_phase_3) {
    auto r = runAot(kStringTargetReturn);
    BEZEL_EXPECT_TRUE(compiledAndPrinted(r, "none\n"));
}

// ===========================================================================
// No target — REJECT (fails on r30-c1, which ran it).
// ===========================================================================

// §14.5 phase 3: "it appears in no self-determined argument position and
// no target context supplies it" — a `var` without a written type supplies
// none. The diagnostic names the target remedy phase 3 itself names.
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           untyped_var_supplies_no_target_cannot_infer_spec_14_5_phase_3) {
    auto r = runAot(kNoTarget);
    BEZEL_EXPECT_TRUE(hasCannotInferDiag(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "no target context supplies it"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "assign it to a typed local"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ===========================================================================
// Named control (1 of at most 2). Green on r30-c1 too (rejected there for
// the initializer's nullability). It goes RED under the plausible wrong
// fix-forward that completes `K` from the target WITHOUT judging the bare
// `null` body against the substituted return slot: K := string makes the
// call's type `string`, the initializer then type-checks, and the lambda
// would hand back a null `string`.
// ===========================================================================

// §14.5 phase 1: "Conversions get their say afterward ... at the §22.6
// applicability check against the substituted signature"; §5.1: `null`
// converts only to a nullable type.
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           control_non_nullable_target_slot_rejects_null_body_spec_14_5_phase_3) {
    auto r = runAot(kBareKeyNonNullableTarget);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "non-nullable"));
}
