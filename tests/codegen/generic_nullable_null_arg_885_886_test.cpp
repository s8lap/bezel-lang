// fix #885 #886 / §14.1 / §14.2: passing a `null` literal for a generic
// method's nullable VALUE-type parameter `T?` crashed the compiler.
//
// Root cause: the arg-type-inference loop in lowerCall typed the `null` arg
// as Object (PrimitiveType::Object, the sema type of a null literal with no
// better concrete type), so T was bound to Object rather than the correct
// concrete type (int, etc.). The binding from a null literal then blocked the
// correct binding from a subsequent concrete argument (e.g. `fallback: T = 42`
// → int) and from the explicit type args (<int>), because the loop skips
// already-bound params. As a result genericSubstSemaTy_["T"] held the Object
// TypeRef; nullableTypeParamConcrete resolved T? → Object? which is NOT a
// nullable value type; the null arg was not wrapped to the {i8,T} struct;
// the specialized function typed the T? param as `ptr`; and lowerCoalesce /
// ICmpEQ in the body saw a PHI / operand type mismatch → SIGABRT (#885) or
// ICmp assertion failure (#886).
//
// Fix: skip null-literal arguments during type-param inference in lowerCall
// (they carry no concrete type information). Added a second guard in
// unifyMethodReturnTypeArg to not bind when concreteTy is an unresolved type
// param or Object (defence in depth).
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// --- #885: `v ?? fallback` where v is `T?` param, null literal passed ---

// Basic #885 repro: GetVal<int>(null, 42) → 42 (null → use fallback).
// Was SIGABRT (PHINode type mismatch in lowerCoalesce). §14.1/§14.2.
BEZEL_TEST(generic_nullable_null_arg_885_886, coalesce_null_literal_returns_fallback) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "static class P\n"
        "  public static method GetVal<T>(v: T?, fallback: T): T\n"
        "    return v ?? fallback;\n"
        "  end GetVal;\n"
        "  public static method Main(): int\n"
        "    var a := GetVal<int>(null, 42);\n"
        "    if a <> 42 then return 1; end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"), 0);
}

// Non-null path: GetVal<int>(x, 42) with x: int? := 7 → 7 (non-null path).
// Was already working; must not regress. §14.1.
BEZEL_TEST(generic_nullable_null_arg_885_886, coalesce_nonnull_var_returns_value) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "static class P\n"
        "  public static method GetVal<T>(v: T?, fallback: T): T\n"
        "    return v ?? fallback;\n"
        "  end GetVal;\n"
        "  public static method Main(): int\n"
        "    var x: int? := 7;\n"
        "    var a := GetVal<int>(x, 42);\n"
        "    if a <> 7 then return 1; end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"), 0);
}

// --- #886: `is null` check + `v!` force-unwrap on `T?` param ---

// Basic #886 repro: Get<int>(null, 5) → 5 (null → fallback path).
// Was SIGABRT (ICmpInst operand type mismatch). §14.1/§14.2.
BEZEL_TEST(generic_nullable_null_arg_885_886, null_guard_returns_fallback) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "static class P\n"
        "  public static method Get<T>(v: T?, fallback: T): T where T: value\n"
        "    if v is null then return fallback; end;\n"
        "    return v!;\n"
        "  end Get;\n"
        "  public static method Main(): int\n"
        "    var r := Get<int>(null, 5);\n"
        "    if r <> 5 then return 1; end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"), 0);
}

// Non-null path: Get<int>(x, 5) with x: int? := 9 → 9 (v! path).
// Must not regress. §14.1.
BEZEL_TEST(generic_nullable_null_arg_885_886, null_guard_nonnull_var_returns_value) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "static class P\n"
        "  public static method Get<T>(v: T?, fallback: T): T where T: value\n"
        "    if v is null then return fallback; end;\n"
        "    return v!;\n"
        "  end Get;\n"
        "  public static method Main(): int\n"
        "    var x: int? := 9;\n"
        "    var r := Get<int>(x, 5);\n"
        "    if r <> 9 then return 1; end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"), 0);
}

// --- CLASS guardrail: T is a class/interface — null stays a pointer ---
// For a class T, `T?` is a nullable reference; null IS the pointer-null
// representation and must NOT be wrapped into a value struct. §14.1.
BEZEL_TEST(generic_nullable_null_arg_885_886, class_type_param_null_coalesces_to_fallback) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Widget\n"
        "  public _n: int := 0;\n"
        "  public constructor(v: int) _n := v; end constructor;\n"
        "end Widget;\n"
        "static class P\n"
        "  public static method GetVal<T>(v: T?, fallback: T): T\n"
        "    return v ?? fallback;\n"
        "  end GetVal;\n"
        "  public static method Main(): int\n"
        "    using var inst := new Widget(77);\n"
        "    var a := GetVal<Widget>(null, inst);\n"
        "    if a._n <> 77 then return 1; end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"), 0);
}

// --- Coalesce with default(T) fallback ---
// `v ?? default(T)` where v is null: result should be default(int) = 0. §14.1.
BEZEL_TEST(generic_nullable_null_arg_885_886, coalesce_null_with_default_T) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "static class P\n"
        "  public static method GetOrDefault<T>(v: T?, unused: T): T\n"
        "    return v ?? default(T);\n"
        "  end GetOrDefault;\n"
        "  public static method Main(): int\n"
        "    var a := GetOrDefault<int>(null, 99);\n"
        "    if a <> 0 then return 1; end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"), 0);
}
