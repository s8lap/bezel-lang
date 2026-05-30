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
