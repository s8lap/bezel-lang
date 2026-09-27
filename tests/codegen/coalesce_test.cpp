// Codegen slice CG39 — null-coalescing `??` (§17.1 L12). `a ?? b` yields
// `a` when it is a non-null reference, otherwise `b`. The right operand is
// evaluated only when the left is null (short-circuit). Reference-typed
// operands; nullable value types are deferred. Observable via exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Body of `C.Main(): int`, wrapped with a helper class `T { V: int }`.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class T public V: int := 0; end T; "
                    "class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// A whole program (for tests that need an auxiliary method).
int runProg(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg39, left_null_yields_right) {
    BEZEL_EXPECT_EQ(run(
        "var a: T? := null; "
        "var b := new T(); b.V := 42; "
        "var r := a ?? b; return r.V;"), 42);
}

BEZEL_TEST(codegen_cg39, left_non_null_yields_left) {
    BEZEL_EXPECT_EQ(run(
        "var x := new T(); x.V := 7; var a: T? := x; "
        "var b := new T(); b.V := 99; "
        "var r := a ?? b; return r.V;"), 7);
}

BEZEL_TEST(codegen_cg39, chained_coalesce) {
    // `a ?? b ?? c` is right-associative; the first non-null wins.
    BEZEL_EXPECT_EQ(run(
        "var a: T? := null; "
        "var b: T? := null; "
        "var c := new T(); c.V := 33; "
        "var r := a ?? b ?? c; return r.V;"), 33);
}

BEZEL_TEST(codegen_cg39, right_not_evaluated_when_left_non_null) {
    // Short-circuit: `Boom()` raises (would surface DivideByZeroException)
    // but is never called because the left is non-null, so the result is the
    // left's V. v0.201 (#2048): the static factory's class-typed return is
    // spelled `owned T` per the declaration-site static-return rule; the body
    // raises on every path so no owned value is produced — the `owned T`
    // return exists only to satisfy the declaration-site rule.
    BEZEL_EXPECT_EQ(runProg(
        "module M; class T public V: int := 0; end T; "
        "class C "
        "  public static method Boom(): owned T "
        "    var z := 1 / 0; var t := new T(); t.V := z; return t; "
        "  end Boom; "
        "  public static method Main(): int "
        "    var x := new T(); x.V := 5; var a: T? := x; "
        "    var r := (if a <> null then a!.V else Boom().V); "
        "    return r; "
        "  end Main; "
        "end C;"), 5);
}

BEZEL_TEST(codegen_cg39, right_evaluated_when_left_null) {
    // Complement: when the left is null the right operand IS evaluated —
    // `Boom()` runs and its DivideByZeroException is observed. v0.201 (#2048):
    // the fresh-producing factory is spelled `owned T` per the declaration-
    // site static-return rule.
    // #4964 (§28.3 `a ?? b` row, K7.3, v0.307): a named local left arm is a
    // borrow, so `a ?? Boom()` with an owned right is a mixed meet and is
    // rejected. The left is therefore a null-returning `owned T?` producer,
    // which makes the meet owned⊓owned (legal). This row still pins what it
    // always did: the left is null, so `Boom()` runs and throws.
    BEZEL_EXPECT_EQ(runProg(
        "module M; class T public V: int := 0; end T; "
        "class C "
        "  public static method Nothing(): owned T? "
        "    return null; "
        "  end Nothing; "
        "  public static method Boom(): owned T "
        "    var z := 1 / 0; return new T(); "
        "  end Boom; "
        "  public static method Main(): int "
        "    try using var r := C.Nothing() ?? Boom(); return 1; "
        "    catch DivideByZeroException => return 7; end; "
        "  end Main; "
        "end C;"), 7);
}
