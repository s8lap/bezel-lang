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
