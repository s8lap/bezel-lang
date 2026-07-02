// Codegen tests for Guid new members — issue #1357 / stdlib §2.6 v0.165.
//
// New members:
//   - Guid.CompareTo(other: Guid): int  (IComparable<Guid>)
//   - Guid.FromByteArray(bytes: Array<byte>): Guid
//
// Note: Bezel uses `=` for equality (not `==`), `and`/`or`/`not` for boolean ops.
// Each test cites the stdlib section and specific behaviour it verifies.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// stdlib §2.6 / #1357: Guid.CompareTo — equal Guids → 0.
BEZEL_TEST(codegen_guid_compar_frombytes_1357, compareto_equal_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var g := Guid.Parse(\"00000000-0000-0000-0000-000000000001\"); "
        "var r := g.CompareTo(Guid.Parse(\"00000000-0000-0000-0000-000000000001\")); "
        "if r = 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.6 / #1357: Guid.CompareTo — Empty is less than non-zero Guid.
BEZEL_TEST(codegen_guid_compar_frombytes_1357, compareto_empty_less_than_nonzero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var g := Guid.Parse(\"00000000-0000-0000-0000-000000000001\"); "
        "var r := Guid.Empty.CompareTo(g); "
        "if r < 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.6 / #1357: Guid.CompareTo — larger > smaller → positive.
BEZEL_TEST(codegen_guid_compar_frombytes_1357, compareto_larger_positive) {
    BEZEL_EXPECT_EQ(run(prog(
        "var small := Guid.Parse(\"00000000-0000-0000-0000-000000000001\"); "
        "var large := Guid.Parse(\"00000000-0000-0000-0000-000000000002\"); "
        "var r := large.CompareTo(small); "
        "if r > 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.6 / #1357: Guid.FromByteArray — 16 zero bytes → Empty Guid.
// `new Array<byte>(16)` is the CAPACITY constructor (Count = 0, stdlib §4.4) —
// the merge-gate length guard correctly rejects it, so the 16 zero elements
// must actually be added. (The sized-fill constructor (size, fill) is #1375,
// not yet implemented.)
BEZEL_TEST(codegen_guid_compar_frombytes_1357, from_byte_array_zeros_equals_empty) {
    BEZEL_EXPECT_EQ(run(prog(
        "var bytes := new Array<byte>(16); "
        "var i := 0; "
        "while i < 16 do bytes.Add(0); i := i + 1; end; "
        "var g := Guid.FromByteArray(bytes); "
        "if g.Equals(Guid.Empty) then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.6 / #1357: FromByteArray "requires Length = 16, ArgumentException
// otherwise" — a short array raises a CATCHABLE ArgumentException before any
// byte is read (merge-gate guard; the unguarded lowering read 16 bytes OOB).
BEZEL_TEST(codegen_guid_compar_frombytes_1357, from_byte_array_short_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "var bytes := new Array<byte>(4); "
        "try "
        "  var g := Guid.FromByteArray(bytes); "
        "catch ArgumentException => return 7; "
        "end; "
        "return 0;"
    )), 7);
}
