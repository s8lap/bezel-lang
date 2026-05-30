// Codegen slice CG50 — `ref` / `out` parameters (§22.3). A ref/out parameter
// is passed by reference: its LLVM type is a pointer to the value type, the
// caller passes the address of the argument lvalue, and reads/writes inside
// the callee go through that pointer — so mutations are visible to the
// caller. Observable via the int exit code. (`out var x` declaring forms
// are deferred.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg50, ref_param_mutation_visible) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Inc(ref x: int) x := x + 1; end Inc; "
        "  public static method Main(): int "
        "    var n := 5; Inc(ref n); return n; "
        "  end Main; "
        "end C;"), 6);
}

BEZEL_TEST(codegen_cg50, out_param_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Get(out x: int) x := 42; end Get; "
        "  public static method Main(): int "
        "    var n := 0; Get(out n); return n; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg50, two_ref_params_swap) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Swap(ref a: int, ref b: int) "
        "    var t := a; a := b; b := t; "
        "  end Swap; "
        "  public static method Main(): int "
        "    var x := 3; var y := 7; Swap(ref x, ref y); return x * 10 + y; "
        "  end Main; "
        "end C;"), 73);
}

BEZEL_TEST(codegen_cg50, ref_reads_and_writes) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Double(ref x: int) x := x * 2; end Double; "
        "  public static method Main(): int "
        "    var n := 21; Double(ref n); return n; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg50, ref_to_a_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N public V: int := 0; end N; "
        "class C "
        "  public static method Inc(ref x: int) x := x + 1; end Inc; "
        "  public static method Main(): int "
        "    var n := new N(); n.V := 41; Inc(ref n.V); return n.V; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg50, by_value_param_unchanged) {
    // A by-value parameter still receives a copy; mutating it does not
    // affect the caller's variable.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Bump(x: int) x := x + 100; end Bump; "
        "  public static method Main(): int "
        "    var n := 5; Bump(n); return n; "
        "  end Main; "
        "end C;"), 5);
}
