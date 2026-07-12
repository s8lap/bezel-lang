// fix #846 / §14.2 / §7.1: a generic method with a nullable VALUE-type parameter
// `T?` (T bound to a value type) crashed the compiler. emitGenericSpecialization
// derived the specialized function's parameter types from the ARGUMENT LLVM types
// rather than the declared (substituted) parameter types, so a bare-`int`
// argument bound to a `T?` parameter left the param typed `i32`. The body,
// however, reads the param as the tagged `{i8, T}` nullable struct (e.g. `v!`),
// so its ExtractValue indexed a non-aggregate → LLVM checkGEPType assertion
// (SIGABRT). The fix wraps a bare/null argument into the `{i8, T}` struct and
// types the param accordingly when the declared parameter is a nullable value
// type under the active substitution.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Force-unwrap `v!` on a generic value-type `T?` param, bare literal arg. The
// bug00 repro (was a checkGEPType assertion crash).
BEZEL_TEST(generic_nullable_value_param_846, force_unwrap_bare_int_arg) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "static class U\n"
        "  public static method ForceUnwrap<T>(v: T?): T where T: value\n"
        "    return v!;\n"
        "  end ForceUnwrap;\n"
        "end U;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    return U.ForceUnwrap<int>(42);\n"   // 42
        "  end Main;\n"
        "end C;\n"), 42);
}

// NOTE: passing an ALREADY-`int?` argument (a nullable VARIABLE rather than a
// bare literal) to the `T?` param is a SEPARATE pre-existing miscompile — it
// reads the tag instead of the value (tracked under #845, the generic-`T?`-param
// cluster). The fix here is provably a no-op for that path (no wrap when the arg
// already matches the `{i8, T}` struct), so it is intentionally not asserted.

// `long` value-type arg (i64, wider than the i32 the body would default to).
BEZEL_TEST(generic_nullable_value_param_846, force_unwrap_long_arg) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "static class U\n"
        "  public static method ForceUnwrap<T>(v: T?): T where T: value\n"
        "    return v!;\n"
        "  end ForceUnwrap;\n"
        "end U;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var r: long := U.ForceUnwrap<long>(5000000000L);\n"
        "    return (1 if r = 5000000000L else 0);\n"  // 1
        "  end Main;\n"
        "end C;\n"), 1);
}

// Instance generic method (this offset = 1) with a `T?` value param + a null
// check, so the param-passing is exercised without a `??`/return path.
BEZEL_TEST(generic_nullable_value_param_846, instance_method_nullable_value_param) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Holder\n"
        "  public method Get<T>(v: T?): int where T: value\n"
        "    if v is null then return 0; end;\n"
        "    return 1;\n"
        "  end Get;\n"
        "end Holder;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var h := new Holder();\n"
        "    return h.Get<int>(9);\n"           // 1 (present)
        "  end Main;\n"
        "end C;\n"), 1);
}
