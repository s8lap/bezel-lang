// Spec-derived tests for issue #1469 — an inline `out var x` bound to a GENERIC
// method's `out T?` parameter where T is a value type (§14.2, §7.10, §22.4).
//
// §22.4: `out var x` declares a new variable inline, typed from the callee's
// `out` parameter. §14.2 / §7.10: inside a generic body `T?` is the nullable
// companion of T; for `where T: value` and T=int the out parameter is `int?`,
// so `out var b` must have static type `int?` — the tagged {i8 hasValue; T}
// value struct.
//
// THE BUG (two layers):
//   1. sema left the out-var local typed as the UNRESOLVED, nullable-stripped
//      type parameter `T` (harmless for a reference type, but for a value type
//      `int?` is a distinct struct). `b = 88` then lowered as ICmp(struct, i32)
//      and aborted the compiler ("both operands to ICmp not same type"). The
//      identical NON-generic `out v: int?`, and a non-nullable generic `out v:
//      T`, compiled fine — so only the generic value-type `out T?` shape broke.
//   2. codegen sized the out slot from the formal `T?` lowered with the i32
//      placeholder ({i8,i32}), correct only for T=int; for `long?`/`bool?`/an
//      enum the specialized callee wrote a wider/narrower struct into the
//      undersized slot and the value read back as garbage.
//
// THE FIX substitutes the method type parameter with the call's type argument
// and canonicalizes the local to the value type's nullable companion (int?),
// then sizes the slot from that concrete type. Both the explicit-type-argument
// (`SetIt<int>`) and argument-inferred (`SetIt(88, …)`) call forms are covered.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

// A generic setter whose `out T?` value parameter is written by a plain
// `v := x` inside the body — the minimal §14.2 shape from issue #1469.
const char* kSetter =
    "module M; "
    "static class U "
    "  public static method SetIt<T>(x: T, out v: T?) where T: value "
    "    v := x; "
    "  end SetIt; "
    "end U; ";
}  // namespace

// The reported repro: EXPLICIT type argument. `out var b` is `int?`; `b = 88`
// is a nullable/int unwrap-and-compare (§7) and yields true — was an ICmp
// type-mismatch compiler abort before the fix.
BEZEL_TEST(generic_out_nullable_1469, explicit_int_arg_compare) {
    BEZEL_EXPECT_EQ(run(
        std::string(kSetter) +
        "class C "
        "  public static method Main(): int "
        "    U.SetIt<int>(88, out var b); "
        "    if b = 88 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// The same shape with the type argument INFERRED from the value argument
// (`SetIt(88, …)` binds T=int) — also aborted before the fix.
BEZEL_TEST(generic_out_nullable_1469, inferred_int_arg_compare) {
    BEZEL_EXPECT_EQ(run(
        std::string(kSetter) +
        "class C "
        "  public static method Main(): int "
        "    U.SetIt(88, out var b); "
        "    if b = 88 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// The out-var is genuinely nullable (`int?`): `b = null` is false and `b!`
// force-unwraps to the delivered payload. (A wrongly-stripped `int` would make
// both `b = null` and `b!` ill-typed.)
BEZEL_TEST(generic_out_nullable_1469, out_var_is_nullable_int) {
    BEZEL_EXPECT_EQ(run(
        std::string(kSetter) +
        "class C "
        "  public static method Main(): int "
        "    U.SetIt<int>(88, out var b); "
        "    if b = null then return -1; end; "
        "    return b!; "  // 88
        "  end Main; "
        "end C;"), 88);
}

// Value delivery for a WIDER value type (`long?`, {i8,i64}): before the slot fix
// the callee's i64 write overflowed the {i8,i32}-sized slot and `b!` read
// garbage. 9L must round-trip.
BEZEL_TEST(generic_out_nullable_1469, long_value_delivered) {
    BEZEL_EXPECT_EQ(run(
        std::string(kSetter) +
        "class C "
        "  public static method Main(): int "
        "    U.SetIt<long>(9L, out var b); "
        "    if b = null then return -1; end; "
        "    var r: long := b!; "
        "    if r = 9L then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// A nullable ENUM value type (`Color?`, tagged {i8,i32}) through the generic —
// the payload compares equal to the delivered member and `= null` is false.
BEZEL_TEST(generic_out_nullable_1469, enum_value_delivered) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color Red := 0, Green := 7; end Color; "
        "static class U "
        "  public static method SetIt<T>(x: T, out v: T?) where T: value "
        "    v := x; "
        "  end SetIt; "
        "end U; "
        "class C "
        "  public static method Main(): int "
        "    U.SetIt<Color>(Color.Green, out var c); "
        "    if c = null then return -1; end; "
        "    if c = Color.Green then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// Regression: a NON-nullable generic `out v: T` value parameter is unaffected —
// the out-var stays the bare value type and the comparison lowers normally.
BEZEL_TEST(generic_out_nullable_1469, non_nullable_generic_out_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class U "
        "  public static method SetIt<T>(x: T, out v: T) where T: value "
        "    v := x; "
        "  end SetIt; "
        "end U; "
        "class C "
        "  public static method Main(): int "
        "    U.SetIt<int>(5, out var b); "
        "    if b = 5 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// Regression: a NON-generic `out v: int?` inline out-var is unaffected (this
// path never crashed; the fix must not disturb it).
BEZEL_TEST(generic_out_nullable_1469, non_generic_out_int_nullable_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class U "
        "  public static method SetIt(x: int, out v: int?) "
        "    v := x; "
        "  end SetIt; "
        "end U; "
        "class C "
        "  public static method Main(): int "
        "    U.SetIt(88, out var b); "
        "    if b = 88 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}
