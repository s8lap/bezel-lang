// fix #3105 — §4.7 / §8.5 / §21.9 / stdlib §2.1, v0.255 — broaden #3079's
// `Console.WriteLine(Unit)` guard to the broader call family. The #3079
// value-demanding guard sat only inside `tryLowerConsoleCall`; the same
// unit-typed value was emitted as a void-typed `CallInst` (`lowerExpr`
// for a unit-returning call returns a non-null `Value*` — the `CallInst`
// IS a `Value`, its type is just `void`), and the broader call args
// loops (`buildOrderedArgs`, the `?.` twin ordered-args loop, the IIFE
// loop, the closure-call loop) saw `if (!v)` false, pushed the void
// value into the args vector, and the IR verifier aborted with
// "Call parameter type does not match function signature" — MEASURED
// on 50eb493 with the issue's `TakeUnit(ReturnsUnit())` repro.
//
// §4.7 admits the boxing of `()` through `Object`; §8.5 gives the
// rendering `"()"`; stdlib §2.1 declares `Console.WriteLine(value:
// Object)`. The non-Console callee (`TakeUnit(u: ())`) is what the
// spec's "unit-typed parameter, unit-typed argument" combination
// asks for, and at the spec's other call-shape sites the same value
// falls into the same trap.
//
// THE FIX detects the void-typed v BEFORE the existing `!v` rescue
// runs (mirroring the `tryLowerConsoleCall` §8.5 sentinel restoration
// the #3104 fix did for the Console arm) and substitutes the §4.7
// unit placeholder byte (`i8 0`, via `unitPlaceholderTy()`) when the
// formal parameter is unit-typed. The side effects of the original
// call (the emitted `CreateCall`) are preserved at every site — only
// the `Value*` the downstream code stored as the call argument is
// replaced. The nullable-unit branch substitutes
// `nullableUnitPresent(formalTy)` — the same `{i8 1, i8 0}` literal
// the existing `unitValueForTarget` path produces — so the
// nullable-unit case (#3095) is also covered.
//
// THE TESTS below pin the rendered output for the THREE shapes the
// issue names: a non-Console callee with a unit-typed argument, the
// `?.` twin of that call, and the closure-call / IIFE shape. The
// literal `()` and the unit-typed-local shapes are pinned too, so
// the four-position axis grid (Console / non-Console / `?.` / IIFE
// × value-typed / unit-typed argument) is fully covered. The
// regression is the IR verifier failure: every shape below
// pre-fix blows up with "Call parameter type does not match function
// signature", and exits 1; post-fix each shape prints the expected
// line and exits 0.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// Shape (i) — non-Console callee with a unit-typed argument. The
// issue's primary repro: `TakeUnit(ReturnsUnit())` drops the
// enclosing call (the IR verifier aborts the compile) for any
// ordinary user method whose parameter is unit-typed. The
// `ReturnsUnit` body is the §21.9 spec-mandated bare `return;`
// (carrying a value is rejected by §21.7).
BEZEL_TEST(codegen_call_arg_unit_typed_rescued_3105,
           non_console_callee_unit_arg_compiles_spec_4_7_8_5) {
    auto r = runAot(
        "module N3105A; "
        "class C "
        "  public static method ReturnsUnit(): () "
        "    return; "
        "  end ReturnsUnit; "
        "  public static method TakeUnit(u: ()): int "
        "    Console.WriteLine(\"TakeUnit called\"); "
        "    return 0; "
        "  end TakeUnit; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"a\"); "
        "    TakeUnit(ReturnsUnit()); "
        "    Console.WriteLine(\"b\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a\nTakeUnit called\nb\n"));
}

// Shape (ii) — the `?.` twin of the same shape. The `?.` form's
// ordered-args loop builds the args by hand (no `buildOrderedArgs`
// call) and runs the same `if (!v) return nullptr;` guard, so the
// void-typed value slips through exactly the same way. Pinned by an
// observable side effect (`TakeUnit`'s `Console.WriteLine`) on both
// non-null and null receivers, so the receiver-null short-circuit
// is also part of the test.
BEZEL_TEST(codegen_call_arg_unit_typed_rescued_3105,
           qst_twin_unit_arg_compiles_spec_7_5_4_7) {
    auto r = runAot(
        "module N3105B; "
        "class C "
        "  public static method ReturnsUnit(): () "
        "    return; "
        "  end ReturnsUnit; "
        "  public method TakeUnit(u: ()): int "
        "    Console.WriteLine(\"TakeUnit called\"); "
        "    return 0; "
        "  end TakeUnit; "
        "  public static method Main(): int "
        "    var o: C := new C(); "
        "    Console.WriteLine(\"a\"); "
        "    o?.TakeUnit(ReturnsUnit()); "
        "    Console.WriteLine(\"b\"); "
        "    var n: C? := null; "
        "    n?.TakeUnit(ReturnsUnit()); "
        "    Console.WriteLine(\"c\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a\nTakeUnit called\nb\nc\n"));
}

// Shape (iii) — the closure-call / IIFE shape. The `fnPtr`.invoke
// sites build their args in their own loop (NOT through
// `buildOrderedArgs`), and the IIFE loop builds its fnTy from the
// actual LLVM arg types, so a void-typed parameter makes the fnTy
// invalid and the IR verifier aborts. The `var f := lambda(u: ()) =>
// ...; f(ReturnsUnit())` shape exercises the closure-call path;
// the IIFE shape is exercised by an `((u: ()) => ...)(ReturnsUnit())`
// inline invocation below.
BEZEL_TEST(codegen_call_arg_unit_typed_rescued_3105,
           closure_call_unit_arg_compiles_spec_4_7_27_1) {
    auto r = runAot(
        "module N3105C; "
        "class C "
        "  public static method ReturnsUnit(): () "
        "    return; "
        "  end ReturnsUnit; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"a\"); "
        "    var f := lambda(u: ()) => Console.WriteLine(\"closure called\"); "
        "    f(ReturnsUnit()); "
        "    Console.WriteLine(\"b\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a\nclosure called\nb\n"));
}

// Shape (iv) — the IIFE shape. A lambda invoked directly at its
// construction site: `((u: ()) => ...)(ReturnsUnit())`. The IIFE
// loop in `lowerCallDispatch` builds the fnTy from the actual
// argument LLVM types, so a void-typed argument makes the parameter
// type invalid and the IR verifier aborts. Pinning it confirms the
// IIFE site's rescue (a separate inner-loop site from the
// closure-call path above) is also armed.
BEZEL_TEST(codegen_call_arg_unit_typed_rescued_3105,
           iife_call_unit_arg_compiles_spec_4_7_27_3) {
    auto r = runAot(
        "module N3105D; "
        "class C "
        "  public static method ReturnsUnit(): () "
        "    return; "
        "  end ReturnsUnit; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"a\"); "
        "    (lambda(u: ()) => Console.WriteLine(\"iife called\"))(ReturnsUnit()); "
        "    Console.WriteLine(\"b\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a\niife called\nb\n"));
}

// Shape (v) — the unit-typed local argument. Same `TakeUnit(u: ())`
// callee, but the argument is a local variable declared `var u: ()`
// rather than a call. The local's value is null (per the existing
// unit-typed local rescue — already covered by #2915/#3095), so
// this is a redundant pin of the existing `if (!v)` rescue path;
// included so a regression to "the new void-typed check misses the
// null-v case" surfaces here.
BEZEL_TEST(codegen_call_arg_unit_typed_rescued_3105,
           unit_typed_local_arg_compiles_spec_4_7_8_5) {
    auto r = runAot(
        "module N3105E; "
        "class C "
        "  public static method TakeUnit(u: ()): int "
        "    Console.WriteLine(\"TakeUnit called\"); "
        "    return 0; "
        "  end TakeUnit; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"a\"); "
        "    var u: () := (); "
        "    TakeUnit(u); "
        "    Console.WriteLine(\"b\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a\nTakeUnit called\nb\n"));
}
