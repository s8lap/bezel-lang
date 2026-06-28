// fix #859 / §5.4: `E(n)` with a compile-time-constant `n` outside the enum's
// underlying type range is a COMPILE ERROR ("a compile error if n is a constant
// the compiler can evaluate"). The enum-cast typecheck now const-folds the
// argument and checks it against the underlying type's range. (The runtime
// out-of-range case raises OverflowException — see the codegen test.)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {
bool hasEnumCastRangeError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("enum cast") != std::string::npos &&
            d.message.find("underlying") != std::string::npos)
            return true;
    return false;
}
}  // namespace

// §5.4: a constant above a byte-backed enum's range is rejected.
BEZEL_TEST(enum_cast_const_range_859, byte_const_above_range_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; enum Color: byte Red := 0; end Color; "
        "class C public static method Main(): int "
        "  var c: Color := Color(300); return int(c); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasEnumCastRangeError(r));
}

// §5.4: a constant above a short-backed enum's range is rejected.
BEZEL_TEST(enum_cast_const_range_859, short_const_above_range_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; enum HttpStatus: short Ok := 200; end HttpStatus; "
        "class C public static method Main(): int "
        "  var s: HttpStatus := HttpStatus(40000); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasEnumCastRangeError(r));
}

// §5.4: a negative constant for an unsigned (byte) underlying is rejected.
BEZEL_TEST(enum_cast_const_range_859, byte_const_negative_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; enum Color: byte Red := 0; end Color; "
        "class C public static method Main(): int "
        "  var c: Color := Color(-1); return int(c); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasEnumCastRangeError(r));
}

// Accepted: an in-range constant cast is not flagged.
BEZEL_TEST(enum_cast_const_range_859, in_range_const_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; enum Color: byte Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color := Color(2); return int(c); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasEnumCastRangeError(r));
}

// fix #1171 / §5.4/§5.1: an enum cast `E(x)` previously range-checked only a
// compile-time-constant VALUE and never validated the argument's TYPE, so an
// enum-to-enum cast or a `double`/`string`/`bool` argument was silently
// accepted and miscompiled (the source bits were reinterpreted as the enum).
// The argument must be an integer value (use `int(e)` to round-trip an enum).
namespace {
bool hasEnumCastArgTypeError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("argument to an enum cast") != std::string::npos)
            return true;
    return false;
}
}  // namespace

// §5.4/§5.1: an enum-typed argument to a *different* enum's cast is rejected —
// it must be converted to the underlying integer first via `int(e)`.
BEZEL_TEST(enum_cast_arg_type_1171, enum_cast_rejects_enum_typed_arg) {
    auto r = parseAndCheckOwnership(
        "module M; enum A X, Y, Z; end A; enum B P, Q, R; end B; "
        "class C public static method Main(): int "
        "  var a: A := A.Y; var b: B := B(a); return int(b); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasEnumCastArgTypeError(r));
}

// §5.4/§5.1: a `double` argument to an enum cast is rejected.
BEZEL_TEST(enum_cast_arg_type_1171, enum_cast_rejects_double_arg) {
    auto r = parseAndCheckOwnership(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c: Color := Color(2.0); return int(c); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasEnumCastArgTypeError(r));
}

// §5.4/§5.1: a `string` argument to an enum cast is rejected.
BEZEL_TEST(enum_cast_arg_type_1171, enum_cast_rejects_string_arg) {
    auto r = parseAndCheckOwnership(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c: Color := Color(\"s\"); return int(c); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasEnumCastArgTypeError(r));
}

// §5.4/§5.1: a `bool` argument to an enum cast is rejected.
BEZEL_TEST(enum_cast_arg_type_1171, enum_cast_rejects_bool_arg) {
    auto r = parseAndCheckOwnership(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c: Color := Color(true); return int(c); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasEnumCastArgTypeError(r));
}

// §5.4/§5.1: an integer-typed argument (the underlying type) is accepted.
BEZEL_TEST(enum_cast_arg_type_1171, enum_cast_accepts_int_arg) {
    auto r = parseAndCheckOwnership(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var n: int := 1; var c: Color := Color(n); return int(c); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasEnumCastArgTypeError(r));
}

// §5.4/§5.1: the explicit `int(e)` round-trip — convert an enum to its
// underlying integer, then cast that integer to another enum — is accepted.
BEZEL_TEST(enum_cast_arg_type_1171, enum_cast_accepts_explicit_int_of_enum) {
    auto r = parseAndCheckOwnership(
        "module M; enum A X, Y, Z; end A; enum B P, Q, R; end B; "
        "class C public static method Main(): int "
        "  var a: A := A.Y; var b: B := B(int(a)); return int(b); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasEnumCastArgTypeError(r));
}

// §5.4/§5.1: a wider integer (long) than the underlying type is accepted at
// compile time — the range is checked at runtime (OverflowException), per the
// §5.4 out-of-range-integer-to-enum rule.
BEZEL_TEST(enum_cast_arg_type_1171, enum_cast_accepts_wider_integer_arg) {
    auto r = parseAndCheckOwnership(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var lv: long := 1; var c: Color := Color(lv); return int(c); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasEnumCastArgTypeError(r));
}
