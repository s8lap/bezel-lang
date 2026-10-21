// Spec-derived regression test for issue #6806.
//
// spec §8.5 (v0.340): `ToString` is invoked implicitly inside string
// interpolation, and a record's generated ToString renders each component by its
// own ToString; stdlib §2.5 (#1114): DateTime.ToString() is
// "yyyy-MM-ddTHH:mm:ss"; stdlib §3.1: Append(value) behaves as
// Append(value.ToString()). `Emitter::emitValueToString`, the one renderer every
// hole, record component, nullable payload and generic body bottoms out in, had
// no DateTime arm and the plain hole list left DateTime out, so each of these
// printed the raw tick count (637000000000000000) instead of
// 2019-07-29T12:26:40.
//
// Each row loops 50 times (a one-shot render could pass by luck of a recycled
// buffer) and prints the last rendering plus the number of iterations that
// disagreed with the literal expected text.
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

const char* kWant = "2019-07-29T12:26:40";

// `body` computes `var s: string := ...` from `dt` / `n` / `p`; the harness
// loops it 50 times against `want`.
std::string prog(const std::string& decls, const std::string& expr,
                 const std::string& want) {
    return "module M; " + decls +
           " class C public static method Main(): int "
           "  var dt := DateTime.FromTicks(637000000000000000L); "
           "  var n: DateTime? := dt; var z: DateTime? := null; "
           "  var p := new P(dt, n); "
           "  var bad := 0; var s := \"\"; var i := 0; "
           "  while i < 50 do "
           "    s := " + expr + "; "
           "    if s <> \"" + want + "\" then bad := bad + 1; end; "
           "    i := i + 1; "
           "  end; "
           "  Console.WriteLine(s + \" \" + bad.ToString()); "
           "  return 0; "
           "end Main; end C; ";
}

const char* kRec = "public record P(At: DateTime, N: DateTime?); ";

void expectOut(const std::string& decls, const std::string& expr,
               const std::string& want) {
    auto r = runAotChecked(prog(decls, expr, want));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, want + " 0\n");
}

} // namespace

// §8.5 interpolation hole (issue #6806 repro).
BEZEL_TEST(codegen_datetime_render_6806, interpolation_hole) {
    expectOut(kRec, "$\"{dt}\"", kWant);
}

// §8.5 + §18.5: the aligned hole renders the same text, padded.
BEZEL_TEST(codegen_datetime_render_6806, interpolation_hole_aligned) {
    expectOut(kRec, "$\"{dt,21}\"", std::string("  ") + kWant);
}

// §8.5: a present DateTime? hole renders the payload by ToString.
BEZEL_TEST(codegen_datetime_render_6806, nullable_hole_present) {
    expectOut(kRec, "$\"{n}\"", kWant);
}

// §8.5: an absent DateTime? hole renders "null".
BEZEL_TEST(codegen_datetime_render_6806, nullable_hole_absent) {
    expectOut(kRec, "$\"{z}\"", "null");
}

// §8.5 / §9.1: a record's generated ToString renders DateTime components.
BEZEL_TEST(codegen_datetime_render_6806, record_tostring_fields) {
    expectOut(kRec, "p.ToString()",
              std::string("P(At: ") + kWant + ", N: " + kWant + ")");
}

// §8.5: the record as an interpolation hole.
BEZEL_TEST(codegen_datetime_render_6806, record_in_hole) {
    expectOut(kRec, "$\"{p}\"",
              std::string("P(At: ") + kWant + ", N: " + kWant + ")");
}

// stdlib §3.1: Append(DateTime?) is Append(value.ToString()) (present and absent).
BEZEL_TEST(codegen_datetime_render_6806, stringbuilder_append_nullable) {
    expectOut(std::string(kRec) +
                  "class H public static method AppendNullable("
                  "a: DateTime?, b: DateTime?): string "
                  "  var sb := new StringBuilder(); sb.Append(a); sb.Append(\"|\"); "
                  "sb.Append(b); return sb.ToString(); end AppendNullable; end H; ",
              "H.AppendNullable(n, z)",
              std::string(kWant) + "|null");
}

// §8.5 / stdlib §3.1: a generic body rendering a T = DateTime value, through
// both Append(v) and an interpolation hole.
BEZEL_TEST(codegen_datetime_render_6806, generic_body_append_and_hole) {
    expectOut(std::string(kRec) +
                  "class G public static method A<T>(v: T): string "
                  "  var b := new StringBuilder(); b.Append(v); return b.ToString(); "
                  "end A; "
                  "public static method H<T>(v: T): string "
                  "  return $\"{v}\"; end H; end G; ",
              "G.A(dt) + \"/\" + G.H(dt)", std::string(kWant) + "/" + kWant);
}

// stdlib §2.5 control: the explicit ToString() was always right.
BEZEL_TEST(codegen_datetime_render_6806, explicit_tostring_control) {
    expectOut(kRec, "dt.ToString()", kWant);
}

// §8.5: Console.WriteLine(dt) writes the ToString text (exact stdout).
BEZEL_TEST(codegen_datetime_render_6806, console_writeline) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var dt := DateTime.FromTicks(637000000000000000L); "
        "  var i := 0; "
        "  while i < 50 do Console.WriteLine(dt); i := i + 1; end; "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    std::string want;
    for (int i = 0; i < 50; ++i) want += std::string(kWant) + "\n";
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}
