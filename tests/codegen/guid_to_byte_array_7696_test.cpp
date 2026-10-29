// Issue #7696 -- the declared `Guid.ToByteArray()` is lowered.
//
// stdlib §2.6 (v0.350), Guid: "public method ToByteArray(): owned Array<byte>;"
// next to "public static method FromByteArray(bytes: Array<byte>): Guid; //
// borrows; raises ArgumentException unless Count = 16" and "CompareTo = lexicographic
// over the 16 canonical big-endian bytes". The array is the 16 canonical bytes
// (the order ToString prints and CompareTo orders), so the pair round-trips.
// Before the fix the call produced no value (degraded via #2434): `.Count` and
// indexing read a zero-initialised stand-in.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// §2.6: Count = 16 and the canonical byte order of a parsed Guid.
BEZEL_TEST(guid_to_byte_array_7696, stdlib_2_6_bytes_are_the_canonical_order) {
    auto r = runAot(prog(
        "var g := Guid.Parse(\"00010203-0405-0607-0809-0a0b0c0d0e0f\"); "
        "using var b := g.ToByteArray(); "
        "Console.WriteLine(b.Count); "
        "var i := 0; "
        "while i < 16 do Console.WriteLine(int(b[i])); i := i + 1; end while; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "16\n0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n");
}

// §2.6: the chained temp spelling from the issue, and the round trip with
// FromByteArray; looped so a leaked array is visible to the ASan leg.
BEZEL_TEST(guid_to_byte_array_7696, stdlib_2_6_chained_and_round_trip_loop) {
    auto r = runAot(prog(
        "Console.WriteLine(Guid.NewGuid().ToByteArray().Count); "
        "var n := 0; var i := 0; "
        "while i < 200 do "
        "  var g := Guid.NewGuid(); "
        "  using var b := g.ToByteArray(); "
        "  if Guid.FromByteArray(b).Equals(g) then n := n + 1; end if; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(n); "
        "Console.WriteLine(Guid.Empty.ToByteArray()[15]); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "16\n200\n0\n");
}

// §2.6 / §28: a plain `var` local of the owned result is reclaimed at scope end.
BEZEL_TEST(guid_to_byte_array_7696, stdlib_2_6_plain_var_local_loop) {
    auto r = runAot(prog(
        "var s := 0; var i := 0; "
        "while i < 200 do "
        "  var b := Guid.NewGuid().ToByteArray(); "
        "  s := s + b.Count; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3200\n");
}
