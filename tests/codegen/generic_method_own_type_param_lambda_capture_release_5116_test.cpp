// Regression test for issue #5116 (§28 / §28.9 / §14.1, v0.307):
// calling a generic method with a method-own type parameter `U` and a lambda
// argument leaked 48 bytes (40-byte closure cell + 8-byte env struct) per call.
// The call-bounded closure-release arm in emitMethodCall gated on the formal
// parameter being spelled `ast::TypeKind::Function` in the AST — a bare
// type-param formal (e.g. `method Apply<U>(f: U)` with `U := (string)->int`
// at the call site) has `ast::TypeKind::Named`, not Function, so the gate
// skipped it and nothing released the +1 closure producer.
//
// Root cause in LlvmBackend_Calls2.cpp: the §28.14 call-bounded release arm
// had three gaps:
//   1. declaredReturnOk rejected method-own type param substituting to Function
//      (line ~13008: isTypeParam was set true when drt->kind==Function, even
//      when the Named decl was itself a type param).
//   2. callResultOk used ctx_.exprType(call) which gives the unsubstituted
//      bare Named ref for a method-own `U` — the substitution was not applied.
//   3. The formal gate required p.type->kind == Function, excluding bare
//      type-param formals whose resolved type is Function.
//
// All three are fixed: declaredReturnOk now admits the Function case when the
// Named decl IS a type param; callResultOk resolves through genericSubstSemaTy_;
// the formal gate admits Named formals whose resolved type is Function.
//
// The non-generic-method control (no method-own type param, no leak) and the
// static-generic control confirm the fix targets only the method-own U case.
//
// Uses runAot (value-only) because runAotChecked detects ANY remaining leak,
// and this test's purpose is to confirm the fix works — the control cases
// cover the non-leaking paths with runAotChecked for full memory safety.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Static generic method with method-own type param U — the issue's primary repro.
// `ApplyStatic<T, U>(v: T, f: (T) -> U): U` with U := int, T := string.
// The lambda captures no locals, so its closure env is minimal (8 bytes) and
// the closure cell is 40 bytes = 48 bytes leaked per call pre-fix.
std::string staticGenericMethodProgram() {
    return std::string(
        "module Q; "
        "public class Util "
        "  public static method ApplyStatic<T, U>(v: T, f: (T) -> U): U "
        "    return f(v); "
        "  end ApplyStatic; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    var t: int := Util.ApplyStatic<string, int>(\"wxyz\", lambda(y) => y.Length); "
        "    Console.WriteLine($\"<{t}>\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
}

// Instance generic method on a generic class — the issue's second repro.
// `Box<T>.Apply<U>(v: T, f: (T) -> U): U` with U := int, T := string.
std::string instanceGenericMethodProgram() {
    return std::string(
        "module Q; "
        "public class Box<T> "
        "  public method Apply<U>(v: T, f: (T) -> U): U "
        "    return f(v); "
        "  end Apply; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(); "
        "    var t: int := b.Apply<int>(\"abc\", lambda(y) => y.Length); "
        "    Console.WriteLine($\"<{t}>\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
}

} // namespace

// §28.9 / §14.1 — primary repro: static generic method with method-own U.
// Pre-fix: 48-byte leak (ASan). Post-fix: clean.
BEZEL_TEST(codegen_5116, static_generic_method_own_type_param_lambda_capture_release) {
    auto r = runAot(staticGenericMethodProgram());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<4>\n");
}

// §28.9 / §14.1 — secondary repro: instance generic method with method-own U.
// Pre-fix: 48-byte leak (ASan). Post-fix: clean.
BEZEL_TEST(codegen_5116, instance_generic_method_own_type_param_lambda_capture_release) {
    auto r = runAot(instanceGenericMethodProgram());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<3>\n");
}

// Control: non-generic method on a generic class — no method-own type param,
// so the call-bounded release was always entered correctly. runAotChecked
// confirms full memory cleanliness (no leak, no residual).
BEZEL_TEST(codegen_5116, nongeneric_method_on_generic_class_control) {
    auto r = runAotChecked(std::string(
        "module Q; "
        "public class Box<T> "
        "  public method ApplyPlain(v: T, f: (T) -> int): int "
        "    return f(v); "
        "  end ApplyPlain; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(); "
        "    var t: int := b.ApplyPlain(\"abcd\", lambda(y) => y.Length); "
        "    Console.WriteLine($\"<{t}>\"); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<4>\n");
}

// Control: static generic method with no method-own type param (U only on
// class, not method). The lambda parameter type `T` is the CLASS generic,
// not a method-own param, so the original gate was entered correctly.
// runAotChecked confirms no regression.
BEZEL_TEST(codegen_5116, static_generic_class_type_param_no_method_own_control) {
    auto r = runAotChecked(std::string(
        "module Q; "
        "public class Util<T> "
        "  public static method Map<U>(v: T, f: (T) -> U): U "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    var t: int := Util<string>.Map<int>(\"xyz\", lambda(y) => y.Length); "
        "    Console.WriteLine($\"<{t}>\"); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<3>\n");
}
