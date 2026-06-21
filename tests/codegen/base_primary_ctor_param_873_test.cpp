// fix #873 / §11.1: base primary-constructor parameters must be visible in the
// base's field initializers when a derived class is constructed.  Before the fix
// the base's positional-param field slots were left at calloc-zero when
// constructed via a derived class, so `class Base(v: int) _field: int := v`
// would see 0 instead of the passed value for both explicit-ctor derived classes
// (`base(v)` chain call) and primary-ctor derived classes (`class D(v): Base(v)`).
//
// The fix has two parts:
//   • codegen (LlvmBackend_Exprs.cpp): prePopulateBasePositionalFields pre-stores
//     base positional-param slots BEFORE emitInstanceInit runs base field inits.
//   • sema (ExprTypecheck.cpp): TypeDecl::primaryBaseCtorArgs expressions are
//     walked during ExprTypecheck so identifier exprSymbols are attached (needed
//     by lowerExpr when codegen evaluates those args).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §11.1, #873: direct construction Base(7) — _field == 7 (regression guard).
BEZEL_TEST(base_primary_ctor_param_873, direct_base_construction_spec_11_1) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Base(v: int) public _field: int := v; end Base;"
        "class C public static method Main(): int"
        "  using var b := new Base(7); if b._field <> 7 then return 1; end; return 0;"
        "end Main; end C;"), 0);
}

// §11.1, #873: explicit ctor derived class — base field initializer sees the
// chained arg value.
BEZEL_TEST(base_primary_ctor_param_873, explicit_derived_base_chain_call_spec_11_1) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Base(v: int) public _field: int := v; end Base;"
        "public class ED: Base"
        "  public constructor(v: int) base(v); end constructor;"
        "end ED;"
        "class C public static method Main(): int"
        "  using var ed := new ED(7); if ed._field <> 7 then return 2; end; return 0;"
        "end Main; end C;"), 0);
}

// §11.1, #873: primary-ctor derived class — base field initializer sees the
// arg from the inheritance clause `class PD(v: int): Base(v)`.
BEZEL_TEST(base_primary_ctor_param_873, primary_derived_inheritance_clause_args_spec_11_1) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Base(v: int) public _field: int := v; end Base;"
        "public class PD(v: int): Base(v) end PD;"
        "class C public static method Main(): int"
        "  using var pd := new PD(7); if pd._field <> 7 then return 3; end; return 0;"
        "end Main; end C;"), 0);
}

// §11.1, #873: multiple base field initializers referencing the same param.
BEZEL_TEST(base_primary_ctor_param_873, multiple_base_fields_from_same_param_spec_11_1) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class MB(v: int)"
        "  public _a: int := v;"
        "  public _b: int := v + 1;"
        "end MB;"
        "public class MD(v: int): MB(v) end MD;"
        "class C public static method Main(): int"
        "  using var md := new MD(5);"
        "  if md._a <> 5 then return 4; end;"
        "  if md._b <> 6 then return 5; end;"
        "  return 0;"
        "end Main; end C;"), 0);
}

// §11.1, #873: expression argument in primary-ctor inheritance clause.
BEZEL_TEST(base_primary_ctor_param_873, expression_arg_in_inheritance_clause_spec_11_1) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Base(v: int) public _field: int := v; end Base;"
        "public class XD(v: int): Base(v * 2) end XD;"
        "class C public static method Main(): int"
        "  using var xd := new XD(3); if xd._field <> 6 then return 6; end; return 0;"
        "end Main; end C;"), 0);
}

// §11.1, #873: multi-level primary chain: L3(v)->L2(v+1)->L1((v+1)*10).
BEZEL_TEST(base_primary_ctor_param_873, multi_level_primary_chain_spec_11_1) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class L1(x: int) public _x: int := x; end L1;"
        "public class L2(x: int): L1(x * 10) public _x2: int := x; end L2;"
        "public class L3(x: int): L2(x + 1) end L3;"
        "class C public static method Main(): int"
        "  using var l3 := new L3(2);"
        "  if l3._x <> 30 then return 7; end;"
        "  if l3._x2 <> 3 then return 8; end;"
        "  return 0;"
        "end Main; end C;"), 0);
}

// §11.1, #873: derived class with own primary fields — no cross-contamination
// between base and derived field initializers.
BEZEL_TEST(base_primary_ctor_param_873, derived_own_fields_no_cross_contamination_spec_11_1) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Base(v: int) public _field: int := v; end Base;"
        "public class WO(v: int): Base(v) public _own: int := v + 100; end WO;"
        "class C public static method Main(): int"
        "  using var wo := new WO(7);"
        "  if wo._field <> 7 then return 9; end;"
        "  if wo._own <> 107 then return 10; end;"
        "  return 0;"
        "end Main; end C;"), 0);
}
