// Regression test for issue #8685 — a `uint` step VARIABLE through an
// `IEnumerable<uint>` parameter drained EMPTY (the `3000000000L` literal step
// was right).
//
// §16.3 (v0.357), verbatim: "`Step` is a signed 64-bit integer, to support
// descending iteration over both signed and unsigned `T`". The step EXPRESSION
// converts to that signed 64-bit `Step` under §5.1's (v0.357) implicit
// conversion table — `byte`/`ushort`/`uint`/`char` → `long` are implicit
// widenings that lose no value — so an UNSIGNED step source must be
// ZERO-EXTENDED (value-preserving) and a signed one sign-extended.
// §16.4 (v0.357): the range's direction is the step's SIGN, and "a step beyond
// that range is **accepted at construction**" (§16.3) with iteration that
// "stops silently, raising no `OverflowException`, before the first value that
// would pass the range's endpoint or fall outside `T`'s representable range".
// §16.5 (v0.357): "The compiler may optimize `for var i in rangeExpr do ... end`
// into a counted loop that bypasses the `IEnumerable` protocol, provided the
// observable behavior is identical" — so the counted loop and the
// `IEnumerable<T>` face of ONE range must agree.
//
// THE DEFECT (measured on the r30-c1 base e69d8b050). The `%bz.range` struct's
// construction site (src/codegen/LlvmBackend_Exprs.cpp, the `ast::RangeExpr`
// lowering) SIGN-EXTENDED the step to the i64 slot unconditionally, so a
// `uint` step of 3000000000 became the NEGATIVE i64 -1294967296. The range's
// direction is the step's sign, so the range iterated DESCENDING and every
// `IEnumerable<T>` face of it drained EMPTY:
//   `ShowI(u0..u9 step 3000000000L)` -> [0,3000000000,]   (the literal is i64)
//   `ShowI(u0..u9 step st)`          -> []                (the `uint` variable)
//   `for var x in e` over the typed  -> nothing
// while the COUNTED LOOP of the same range answered [0,3000000000,] — the
// counted-loop lowering already zero-extended an unsigned step, so the two
// faces of ONE range disagreed (§16.5). The `Step` PROPERTY and the record's
// `ToString` read the same slot and reported the negative step (`Step:
// -1294967296`, `IsEmpty: True`).
//
// Each row prints the counted loop AND the `IEnumerable<T>` face of the SAME
// range, and they must agree; the signed-source and `long`-source rows are
// in-row CONTROLS that catch a plausible wrong fix (a fix that zero-extends
// EVERY step source, or one that truncates the step to 32 bits).
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's own face, under the issue's own name: `ShowI(xs:
// IEnumerable<uint>)`. One class per element type (a module may not declare the
// same class twice).
std::string showI(const std::string& cls, const std::string& ty) {
    return "public class " + cls +
           "  public static method ShowI(xs: IEnumerable<" + ty + ">): string "
           "    var s := \"\"; "
           "    for var x in xs do s := s + $\"{x},\"; end; "
           "    return $\"[{s}]\"; "
           "  end ShowI; "
           "end " + cls + "; ";
}

JitResult run(const std::string& mod, const std::string& helpers,
              const std::string& body) {
    return runAotChecked(std::string("module ") + mod + "; " + helpers +
                         "public class Program public static method Main(): int " +
                         body + " return 0; end Main; end Program;");
}

}  // namespace

// §16.3 (v0.357): the issue's own repro, verbatim (only the module name is
// renamed so the rows do not share a module): a `uint` step VARIABLE through an
// `IEnumerable<uint>` parameter, through the typed formal and through a
// local-decl `IEnumerable<uint>`. The literal `3000000000L` step — the
// issue's control — must keep working (it is already a 64-bit value): a
// plausible wrong fix that TRUNCATES the step to 32 bits would break it.
BEZEL_TEST(codegen_range_step_unsigned_variable_8685,
           issue_repro_uint_step_variable_spec_16_3) {
    JitResult r = run("M8685repro", showI("H", "uint"),
                      "  var u0: uint := 0; var u9: uint := 4000000000; "
                      "var st: uint := 3000000000; "
                      "Console.WriteLine(H.ShowI(u0..u9 step 3000000000L)); "
                      "Console.WriteLine(H.ShowI(u0..u9 step st)); "
                      "var e: IEnumerable<uint> := u0..u9 step st; "
                      "for var x in e do Console.WriteLine(x); end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("[0,3000000000,]\n[0,3000000000,]\n0\n"
                                "3000000000\n"));
}

// §16.3 (v0.357) + §16.5 (v0.357): EVERY unsigned width of a step VARIABLE,
// each against the counted loop of the same range. `byte 200` (above byte's
// signed midpoint), `ushort 40000` (above ushort's) and `uint 3000000000`
// (above uint's) were sign-extended into negative steps and drained empty
// through the `IEnumerable<T>` face while the counted loop answered
// [0,200,] / [0,40000,] / [0,3000000000,]. The `ulong` step is already a
// 64-bit value (the base agrees with it) and the SIGNED step sources keep
// their sign (§16.4) — the two in-row controls, which catch a wrong fix that
// zero-extends EVERY step source (the negative step would become a huge
// positive one) or one that truncates the step to 32 bits (the `ulong` and
// `long` steps would be cut).
BEZEL_TEST(codegen_range_step_unsigned_variable_8685,
           unsigned_step_widths_spec_16_3) {
    JitResult r = run("M8685widths", showI("HB", "byte") +
                                        showI("HS", "ushort") +
                                        showI("HU", "uint") +
                                        showI("HL", "ulong"),
                      "  var b0: byte := 0; var b9: byte := 255; "
                      "var bst: byte := 200; "
                      "Console.WriteLine(HB.ShowI(b0..b9 step bst)); "
                      "var bs := \"\"; "
                      "for var x in b0..b9 step bst do bs := bs + $\"{x},\"; end; "
                      "Console.WriteLine($\"[{bs}]\"); "
                      "var s0: ushort := 0; var s9: ushort := 65535; "
                      "var sst: ushort := 40000; "
                      "Console.WriteLine(HS.ShowI(s0..s9 step sst)); "
                      "var cs := \"\"; "
                      "for var x in s0..s9 step sst do cs := cs + $\"{x},\"; end; "
                      "Console.WriteLine($\"[{cs}]\"); "
                      "var u0: uint := 0; var u9: uint := 4000000000; "
                      "var st: uint := 3000000000; "
                      "Console.WriteLine(HU.ShowI(u0..u9 step st)); "
                      "var us := \"\"; "
                      "for var x in u0..u9 step st do us := us + $\"{x},\"; end; "
                      "Console.WriteLine($\"[{us}]\"); "
                      "var g0: ulong := 0; var g9: ulong := 4000000000; "
                      "var gst: ulong := 3000000000; "
                      "Console.WriteLine(HL.ShowI(g0..g9 step gst)); "
                      "var b3: byte := 3; var u3: uint := 3; "
                      "var neg: int := -1; var negs: short := -1; "
                      "var negl: long := -1; "
                      "Console.WriteLine(HB.ShowI(b3..b0 step neg)); "
                      "Console.WriteLine(HB.ShowI(b3..b0 step negs)); "
                      "Console.WriteLine(HU.ShowI(u3..u0 step negl));");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("[0,200,]\n[0,200,]\n"
                                "[0,40000,]\n[0,40000,]\n"
                                "[0,3000000000,]\n[0,3000000000,]\n"
                                "[0,3000000000,]\n"
                                "[3,2,1,0,]\n[3,2,1,0,]\n[3,2,1,0,]\n"));
}

// §16.3 (v0.357): `Range<T>` declares `public readonly property Step: long`,
// so the property and the record's own `ToString` report the SAME step the
// iteration uses. The sign-extended slot made both report the negative step
// (-1294967296 for the `uint` 3000000000, -56 for the `byte` 200, -25536 for
// the `ushort` 40000), and the `ToString` computed `IsEmpty: True` from the
// step's (wrong) sign.
BEZEL_TEST(codegen_range_step_unsigned_variable_8685,
           step_property_and_tostring_spec_16_3) {
    JitResult r = run("M8685prop", "",
                      "  var u0: uint := 0; var u9: uint := 4000000000; "
                      "var st: uint := 3000000000; "
                      "var r: Range<uint> := u0..u9 step st; "
                      "Console.WriteLine($\"{r.Step}\"); "
                      "Console.WriteLine($\"{r}\"); "
                      "var b0: byte := 0; var b9: byte := 255; "
                      "var bst: byte := 200; "
                      "var rb: Range<byte> := b0..b9 step bst; "
                      "Console.WriteLine($\"{rb.Step}\"); "
                      "var s0: ushort := 0; var s9: ushort := 65535; "
                      "var sst: ushort := 40000; "
                      "var rs: Range<ushort> := s0..s9 step sst; "
                      "Console.WriteLine($\"{rs.Step}\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("3000000000\n"
                                "Range(Start: 0, End: 4000000000, "
                                "Step: 3000000000, IsEndInclusive: True, "
                                "IsOpenEnded: False, IsEmpty: False)\n"
                                "200\n"
                                "40000\n"));
}

// §16.3 (v0.357) + §16.4 (v0.357): "Step is independent of T's range, so a
// step beyond that range is **accepted at construction**", and iteration
// "stops silently, raising no `OverflowException`, before the first value that
// would pass the range's endpoint or fall outside `T`'s representable range".
// A `uint` step of 3000000000 over `0..3` yields only `0` — the sign-extended
// step made the range descend and drain empty instead. The `3000000000L`
// literal is the in-row control that catches a fix which truncates the step.
BEZEL_TEST(codegen_range_step_unsigned_variable_8685,
           step_beyond_the_element_range_spec_16_3) {
    JitResult r = run("M8685beyond", showI("H", "uint"),
                      "  var u0: uint := 0; var u9: uint := 4000000000; "
                      "var st: uint := 3000000000; "
                      "Console.WriteLine(H.ShowI(u0..3 step st)); "
                      "var s := \"\"; "
                      "for var x in u0..3 step st do s := s + $\"{x},\"; end; "
                      "Console.WriteLine($\"[{s}]\"); "
                      "Console.WriteLine(H.ShowI(u0..u9 step 3000000000L));");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("[0,]\n[0,]\n[0,3000000000,]\n"));
}
