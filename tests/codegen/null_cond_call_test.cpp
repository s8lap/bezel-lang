// Codegen slice CG42 — null-conditional call `x?.M(args)` (§7.5). When the
// callee is a null-conditional member access, the call is skipped if the
// receiver is null (no effect / null result); otherwise the method is
// invoked. Void-returning calls (the `user?.Log(...)` idiom) and
// reference-returning calls are covered; value-typed results are deferred.
// Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int runProg(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg42, void_call_runs_when_non_null) {
    // Two `?.` calls on a non-null receiver both run (each bumps V).
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method Bump() V := V + 1; end Bump; "
        "end N; "
        "class C public static method Main(): int "
        "  var n: N? := new N(); n?.Bump(); n?.Bump(); return n!.V; "
        "end Main; end C;"), 2);
}

BEZEL_TEST(codegen_cg42, void_call_skipped_when_null) {
    // A `?.` call on a null receiver is skipped — Bump (which would deref
    // `this`) is never entered, so the program completes and returns 7.
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method Bump() V := V + 1; end Bump; "
        "end N; "
        "class C public static method Main(): int "
        "  var n: N? := null; n?.Bump(); return 7; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg42, ref_returning_call_non_null) {
    // A reference-returning `?.` call on a non-null receiver yields the
    // method's result.
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method Self(): N => this; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); n.V := 5; var nn: N? := n; "
        "  var r := nn?.Self(); return r!.V; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg42, ref_returning_call_null_yields_null) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public method Self(): N => this; "
        "end N; "
        "class C public static method Main(): int "
        "  var nn: N? := null; var r := nn?.Self(); "
        "  if r = null then return 8; end; return 0; "
        "end Main; end C;"), 8);
}

BEZEL_TEST(codegen_cg42, call_with_arguments) {
    // Arguments are evaluated and passed when the call runs.
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method Add(d: int) V := V + d; end Add; "
        "end N; "
        "class C public static method Main(): int "
        "  var n: N? := new N(); n?.Add(40); n?.Add(2); return n!.V; "
        "end Main; end C;"), 42);
}
