// Codegen slice CG45 — default parameter values (§22.7). A call that omits
// a trailing parameter with a default fills it with the default-value
// expression, evaluated at the call site. Observable via the int exit code.
// (Named arguments, which reorder/skip, are a separate slice.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg45, default_used_when_omitted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Add(a: int, b: int := 10): int => a + b; "
        "  public static method Main(): int => Add(5); "
        "end C;"), 15);
}

BEZEL_TEST(codegen_cg45, explicit_overrides_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Add(a: int, b: int := 10): int => a + b; "
        "  public static method Main(): int => Add(5, 20); "
        "end C;"), 25);
}

BEZEL_TEST(codegen_cg45, multiple_defaults_all_omitted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := 2, c: int := 3): int "
        "    => a + b + c; "
        "  public static method Main(): int => F(10); "
        "end C;"), 15);
}

BEZEL_TEST(codegen_cg45, multiple_defaults_partial) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := 2, c: int := 3): int "
        "    => a + b + c; "
        "  public static method Main(): int => F(10, 20); "
        "end C;"), 33);
}

BEZEL_TEST(codegen_cg45, instance_method_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N "
        "  public V: int := 0; "
        "  public method Add(d: int := 5) V := V + d; end Add; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); n.Add(); n.Add(); return n.V; "
        "end Main; end C;"), 10);
}

// §22.5: "a default-value expression may ... read other parameters appearing
// earlier in the same parameter list (e.g. M(a: int, b: int = a + 1) is
// permitted)." The default must evaluate against THIS call's earlier argument
// values, not a zero/garbage slot. (Regression: the default `b := a` produced
// 0 because `a` inside the default was lowered in a scope where the earlier
// parameter wasn't bound to its argument value.)
BEZEL_TEST(codegen_cg45, default_reads_earlier_param_spec_22_5) {
    // F(5): b defaults to a (= 5). Without the fix this returned 0.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := a): int => b; "
        "  public static method Main(): int => F(5); "
        "end C;"), 5);
}

BEZEL_TEST(codegen_cg45, explicit_overrides_earlier_param_default_spec_22_5) {
    // F(5, 9): b is supplied (= 9), the default is not taken.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := a): int => b; "
        "  public static method Main(): int => F(5, 9); "
        "end C;"), 9);
}

BEZEL_TEST(codegen_cg45, default_earlier_param_in_expression_spec_22_5) {
    // §22.5 example: M(a: int, b: int = a + 1). F(5) → b = 6.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := a + 1): int => b; "
        "  public static method Main(): int => F(5); "
        "end C;"), 6);
}

BEZEL_TEST(codegen_cg45, default_literal_still_works_control_spec_22_5) {
    // Control: a literal default must keep working alongside the earlier-param
    // path (it ignores the temporary earlier-param bindings).
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := 99): int => b; "
        "  public static method Main(): int => F(5); "
        "end C;"), 99);
}

BEZEL_TEST(codegen_cg45, default_chained_earlier_params_spec_22_5) {
    // §22.5: each default reads parameters strictly EARLIER in the list. Here
    // b := a and c := b chain; F(5) → a=5, b=5, c=5, sum 15.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := a, c: int := b): int "
        "    => a + b + c; "
        "  public static method Main(): int => F(5); "
        "end C;"), 15);
}

BEZEL_TEST(codegen_cg45, ctor_default_reads_earlier_param_spec_22_5) {
    // §22.5 applies to constructors too: the default for `b` reads the
    // earlier ctor parameter `a`. new C(7) → V = a + b = 7 + 7 = 14.
    BEZEL_EXPECT_EQ(run(
        "module M; class N "
        "  public V: int := 0; "
        "  public constructor(a: int, b: int := a) V := a + b; end constructor; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(7); return n.V; "
        "end Main; end C;"), 14);
}

// §22.5 / §17.11 (issue #1163): `default(T)` is a permitted default-value
// expression. For a REFERENCE / nullable-reference parameter (`R?`) the omitted
// argument is the null reference (ptr null). Regression: the omitted-arg default
// `default(R?)` was lowered as a bare `i32 0` (the i32 placeholder for an
// unresolved TypeRef — the parameter-default expression's inner type is not
// walked by type resolution) and passed to the `ptr` parameter, so codegen
// aborted with "Call parameter type does not match function signature". The
// fix lowers the omitted `default(T)` at the parameter's own (resolved) lowered
// type. `U.M()` takes the default → r is null → returns 5.
BEZEL_TEST(codegen_cg45, default_ref_type_default_arg_omitted_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R public V: int; public constructor(v: int) V := v; end; end R; "
        "static class U public static method M(r: R? := default(R?)): int "
        "  if r = null then return 5; end; return r!.V; "
        "end M; end U; "
        "class C public static method Main(): int => U.M(); "
        "end C;"), 5);
}

BEZEL_TEST(codegen_cg45, default_ref_type_explicit_arg_overrides_spec_22_5) {
    // The explicit reference argument is passed; the `default(R?)` default is
    // not taken. U.M(new R(9)) → r!.V = 9.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R public V: int; public constructor(v: int) V := v; end; end R; "
        "static class U public static method M(r: R? := default(R?)): int "
        "  if r = null then return 5; end; return r!.V; "
        "end M; end U; "
        "class C public static method Main(): int => U.M(new R(9)); "
        "end C;"), 9);
}

BEZEL_TEST(codegen_cg45, default_nullable_value_type_default_arg_spec_22_5) {
    // `default(int?)` is the null nullable-value (the tagged {i8,int} zero).
    // P() takes the default → x is null → returns 11.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class U public static method P(x: int? := default(int?)): int "
        "  if x = null then return 11; end; return x!; "
        "end P; end U; "
        "class C public static method Main(): int => U.P(); "
        "end C;"), 11);
}

BEZEL_TEST(codegen_cg45, default_value_type_default_arg_spec_22_5) {
    // `default(int)` is the value-type zero (0); the value-type path is
    // unaffected by the #1163 reference-type fix. N() → x = 0, +7 = 7.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class U public static method N(x: int := default(int)): int "
        "  => x + 7; "
        "end U; "
        "class C public static method Main(): int => U.N(); "
        "end C;"), 7);
}

// True iff the front end rejected `src` during sema with a diagnostic whose
// message contains `needle` (and the program did not run).
namespace {
bool rejectedWith(const std::string& src, const std::string& needle) {
    auto r = runJit(src);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
} // namespace

BEZEL_TEST(codegen_cg45, default_forward_param_ref_rejected_spec_22_5) {
    // §22.5 (issue #1160): a default may read only parameters declared EARLIER
    // in the same list — a forward reference (`a := b` where `b` is later) reads
    // an uninitialized slot and must be a compile error, not silently accepted.
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C "
        "  public static method M(a: int := b, b: int := 7): int => a * 100 + b; "
        "  public static method Main(): int => M(); "
        "end C;",
        "§22.5"));
}

BEZEL_TEST(codegen_cg45, default_self_param_ref_rejected_spec_22_5) {
    // §22.5 (issue #1160): a default may not reference the parameter it belongs
    // to (`a := a`) — that is a self/forward reference into an uninitialized
    // slot.
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C "
        "  public static method M(a: int := a): int => a; "
        "  public static method Main(): int => M(); "
        "end C;",
        "§22.5"));
}

BEZEL_TEST(codegen_cg45, ctor_default_forward_param_ref_rejected_spec_22_5) {
    // §22.5 (issue #1160): the forward-reference rule applies to constructors
    // too — the default for `a` may not read the later ctor parameter `b`.
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class N "
        "  public V: int := 0; "
        "  public constructor(a: int := b, b: int := 7) V := a + b; "
        "  end constructor; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); return n.V; "
        "end Main; end C;",
        "§22.5"));
}
