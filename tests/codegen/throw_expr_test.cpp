// Codegen slice CG40 — `throw` in expression position (§17.12). A throw
// expression has the bottom type and diverges (it branches to the unwind
// target), so it is valid in any expression position. The lowering marks
// the branch as diverged; phi-building consumers (the conditional `if/else`
// expression and `??`) exclude the diverged branch from the join. The two
// idiomatic positions — `cond ? x : throw E` and `value ?? throw E` — are
// exercised here, both for the not-taken (no throw) and taken (throws)
// directions. Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int runProg(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

const char* kE = "module M; class E end E; class T public V: int := 0; end T; ";

} // namespace

// --- `??` rhs ---------------------------------------------------------------

BEZEL_TEST(codegen_cg40, coalesce_throw_not_taken) {
    // Left non-null → result is the left; the `throw` is not evaluated.
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var x := new T(); x.V := 5; var a: T? := x; "
        "  var r := a ?? throw new E(); return r.V; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg40, coalesce_throw_taken) {
    // Left null → the `throw` runs and its exception is catchable.
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var a: T? := null; "
        "  try var r := a ?? throw new E(); return 1; "
        "  catch E => return 9; end; "
        "end Main; end C;"), 9);
}

// --- conditional branch -----------------------------------------------------

BEZEL_TEST(codegen_cg40, conditional_else_throw_not_taken) {
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var ok := true; var r := (5 if ok else throw new E()); return r; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg40, conditional_else_throw_taken) {
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var ok := false; "
        "  try var r := (5 if ok else throw new E()); return r; "
        "  catch E => return 9; end; "
        "end Main; end C;"), 9);
}

BEZEL_TEST(codegen_cg40, conditional_then_throw_not_taken) {
    // throw in the then-branch (parenthesized so it does not swallow the
    // conditional); condition false → else value used, throw not evaluated.
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var bad := false; "
        "  var r := ((throw new E()) if bad else 7); return r; "
        "end Main; end C;"), 7);
}

// --- statement position -----------------------------------------------------

BEZEL_TEST(codegen_cg40, bare_throw_expression_statement) {
    // A `throw` expression used as a statement diverges; the catch observes
    // it and the post-throw `return 1` is correctly treated as unreachable.
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  try throw new E(); return 1; catch E => return 4; end; "
        "end Main; end C;"), 4);
}

// --- expression-bodied method whose ENTIRE body is a `throw` ----------------
// §17.12 (throw is an expression, valid in any expression position) + §22
// (expression-bodied methods `=> expr`). FINDING [3]: `=> throw E` segfaulted
// bezelc — lowerExpr of the ThrowExpr terminated the block (and set diverged_),
// then the expr-body path unconditionally appended a `ret` into the now-
// terminated block, giving the function two terminators (malformed IR that
// crashed the optimizer). The fix checks diverged_ after lowering the body
// expression and skips the implicit return when the block is already ended.

BEZEL_TEST(codegen_cg40, expr_body_method_throw_caught) {
    // The whole body is `throw E`. Calling it inside a try/catch must (a) NOT
    // crash the compiler and (b) propagate the exception to the handler.
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C "
        "  public static method Bad(): int => throw new E(); "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try var x := Bad(); catch E => caught := 42; end; "
        "    return caught; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg40, expr_body_method_throw_not_called_returns_other) {
    // The diverging expr-bodied method is declared but never called; the rest of
    // the program must compile and run normally (regression guard: the throwing
    // method's body must still emit valid IR even when unused).
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C "
        "  public static method Bad(): int => throw new E(); "
        "  public static method Main(): int => 7; "
        "end C;"), 7);
}

// --- CONTROL: a normal expression-bodied method still returns correctly -----
// The termination check must be FALSE for a non-diverging body, so emitReturn
// runs exactly as before. These pin that the fix is scoped to the throw case.

BEZEL_TEST(codegen_cg40, expr_body_method_const_returns) {
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C "
        "  public static method Zero(): int => 0; "
        "  public static method Main(): int => Zero(); "
        "end C;"), 0);
}

BEZEL_TEST(codegen_cg40, expr_body_method_arith_returns) {
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C "
        "  public static method Add(a: int, b: int): int => a + b; "
        "  public static method Main(): int => Add(40, 2); "
        "end C;"), 42);
}

// --- expression-bodied PROPERTY getter whose body is a `throw` --------------
// §23.3 (block-form / expression-form property accessors) + §17.12. Property-
// accessor exception PROPAGATION correctness is a separate lane (FINDING [4]);
// this test's contract is narrower: the compiler must NOT CRASH lowering an
// expr-body getter whose whole body is a throw. `ran` (compiled + JIT-executed
// without crashing) is the assertion; the exit value is not pinned here.
BEZEL_TEST(codegen_cg40, expr_body_property_getter_throw_no_compiler_crash) {
    auto r = runJit(std::string(kE) +
        "class C "
        "  public property Bad: int "
        "    get => throw new E(); "
        "  end Bad; "
        "  public static method Main(): int => 0; "
        "end C;");
    // The defining program never reads C.Bad, so Main runs to completion; the
    // point is that emitting the getter body did not produce malformed IR.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
