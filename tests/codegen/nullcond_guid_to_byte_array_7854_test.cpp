// Issue #7854 -- `g?.ToByteArray()` on a `Guid?` lowers.
//
// stdlib §2.6 (v0.350), Guid: "public method ToByteArray(): owned Array<byte>;"
// (the 16 canonical big-endian bytes). spec §7.5 (v0.350): `g?.M()` is `null` for
// a null `g`, otherwise the plain `g.M()` lifted to the nullable form of M's
// type -- `Array<byte>?` here. #7696 lowered the `.` spelling only; the `?.`
// spelling on the `Guid?` receiver produced no value (the #2434 / #3079
// backstop dropped the store). The siblings `g?.CompareTo(h)` and the
// `?.Count` chain ride the same lowering.

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

// §7.5 / stdlib §2.6: non-null receiver yields the 16 canonical bytes; null
// receiver yields null.
BEZEL_TEST(nullcond_guid_to_byte_array_7854, spec_7_5_present_and_absent) {
    auto r = runAot(prog(
        "var g: Guid? := Guid.Parse(\"00010203-0405-0607-0809-0a0b0c0d0e0f\"); "
        "var gn: Guid? := null; "
        "var b := g?.ToByteArray(); "
        "if b <> null then Console.WriteLine(b.Count); Console.WriteLine(int(b[0])); "
        "Console.WriteLine(int(b[15])); end if; "
        "var bn := gn?.ToByteArray(); "
        "if bn = null then Console.WriteLine(\"null\"); end if; "
        "Console.WriteLine(g?.ToByteArray()?.Count ?? -1); "
        "Console.WriteLine(gn?.ToByteArray()?.Count ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "16\n0\n15\nnull\n16\n-1\n");
}

// §28 / stdlib §2.6: the owned array is reclaimed on every iteration. Looped
// 100x so a leaked array is visible to the ASan leg.
BEZEL_TEST(nullcond_guid_to_byte_array_7854, stdlib_2_6_owned_array_is_reclaimed_loop) {
    auto r = runAot(prog(
        "var s := 0; var i := 0; "
        "while i < 100 do "
        "  var h: Guid? := Guid.NewGuid(); "
        "  var hb := h?.ToByteArray(); "
        "  if hb <> null then s := s + hb.Count; end if; "
        "  s := s + (h?.ToByteArray()?.Count ?? 0); "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3200\n");
}

// §7.5 / stdlib §2.6: the sibling builtin Guid method `CompareTo` through `?.`.
BEZEL_TEST(nullcond_guid_to_byte_array_7854, spec_7_5_guid_compareto_sibling) {
    auto r = runAot(prog(
        "var g: Guid? := Guid.Parse(\"00010203-0405-0607-0809-0a0b0c0d0e0f\"); "
        "var h: Guid? := Guid.Parse(\"00010203-0405-0607-0809-0a0b0c0d0e10\"); "
        "var gn: Guid? := null; "
        "Console.WriteLine(g?.CompareTo(g!) ?? 7); "
        "Console.WriteLine(gn?.CompareTo(g!) ?? 7); "
        "if (g?.CompareTo(h!) ?? 7) < 0 then Console.WriteLine(\"less\"); end if; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n7\nless\n");
}
