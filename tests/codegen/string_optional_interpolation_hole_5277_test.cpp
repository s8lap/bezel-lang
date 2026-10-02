// Codegen / refcounting — issue #5277: interpolating a `string?` slot that
// owns a +1 aborted with "refcount over-release (string)".
//
// Spec: §18.5 (string interpolation: a hole is rendered via `ToString`, a
// null `string?` renders "null"), §28.9 (runtime-managed types: a value is
// released exactly once and never freed while reachable). Spec revision
// v0.313.
//
// Root: the nullable-REFERENCE arm of the interpolation lowering renders a
// `string?` hole as PHI(slot value, "null" literal). The live arm is the
// identity render (`string.ToString()` is the string itself), but the PHI is a
// new llvm::Value distinct from the lowered operand, so appendPart's
// "`pv != srcEv` means a fresh render" rule released it. For a slot READ that
// release dropped the reference the slot owns: a slot holding a +1 (producer
// initialiser, a #5253-retained assigned parameter, a no-initializer local)
// was then released again at scope end / on overwrite (the abort), and a
// field's string was freed while still reachable (use-after-free). The
// non-nullable `string` hole is the `pv == srcEv` borrow and is never
// released unless it is a +1 producer; the `string?` slot read now follows
// the same rule.
//
// Every row loops 200 times, builds each string at RUNTIME so the refcount is
// real, and runs under runAotChecked (generated-code ASan + LeakSanitizer: an
// over-release abort, use-after-free or leak makes `ran` false). The oracle is
// the full stdout. Pre-fix every row except the producer guard failed.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr int kIters = 200;

std::string run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// Length of "<one-char tag><i>".
int tagLen(int i) { return 1 + static_cast<int>(std::to_string(i).size()); }

// `body` runs once per iteration of `for var i in 0..<200` and adds to `t`.
std::string prog(const std::string& helpers, const std::string& body) {
    return "module M; "
           "public class Program " + helpers +
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0..<200 do " + body +
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// The issue's base repro: a `string?` local initialised from a +1 producer.
// Pre-fix (r23-c28 too): exit 134, "refcount over-release (string)".
BEZEL_TEST(codegen_string_optional_interpolation_hole_5277,
           initialised_local_hole_spec_18_5_28_9) {
    int want = 0;
    for (int i = 0; i < kIters; ++i) want += tagLen(i) + 2;
    BEZEL_EXPECT_EQ(run(prog("",
        "      var p: string? := \"w\" + i.ToString(); "
        "      var s := $\"{p}-x\"; "
        "      t := t + s.Length; ")), std::to_string(want) + "\n");
}

// The reviewer's #5253 regression: an assigned by-value `string?` parameter
// (retained by retainAssignedStringParams, released at scope end).
BEZEL_TEST(codegen_string_optional_interpolation_hole_5277,
           assigned_param_hole_spec_18_5_28_9) {
    int want = 0;
    for (int i = 0; i < kIters; ++i) want += tagLen(i) + 2;
    BEZEL_EXPECT_EQ(run(prog(
        "  static method Rep(p: string?, i: int): int "
        "    p := \"w\" + i.ToString(); "
        "    var s := $\"{p}-x\"; "
        "    return s.Length; "
        "  end Rep; ",
        "      t := t + Rep(null, i); ")), std::to_string(want) + "\n");
}

// A no-initializer `string?` local assigned a producer (§7.9).
BEZEL_TEST(codegen_string_optional_interpolation_hole_5277,
           no_init_local_hole_spec_18_5_28_9) {
    int want = 0;
    for (int i = 0; i < kIters; ++i) want += tagLen(i) + 2;
    BEZEL_EXPECT_EQ(run(prog("",
        "      var p: string?; "
        "      p := \"w\" + i.ToString(); "
        "      var s := $\"{p}-x\"; "
        "      t := t + s.Length; ")), std::to_string(want) + "\n");
}

// `var u := $"{p}"`: the hole is the whole interpolation.
BEZEL_TEST(codegen_string_optional_interpolation_hole_5277,
           sole_hole_var_u_spec_18_5_28_9) {
    int want = 0;
    for (int i = 0; i < kIters; ++i) want += tagLen(i);
    BEZEL_EXPECT_EQ(run(prog("",
        "      var p: string? := \"w\" + i.ToString(); "
        "      var u := $\"{p}\"; "
        "      t := t + u.Length; ")), std::to_string(want) + "\n");
}

// A null-valued `string?` hole renders "null" (§18.5), both for a null local
// and for a parameter that is assigned on even iterations only.
BEZEL_TEST(codegen_string_optional_interpolation_hole_5277,
           null_valued_hole_spec_18_5_28_9) {
    int want = 0;
    for (int i = 0; i < kIters; ++i)
        want += 6 + (i % 2 == 0 ? tagLen(i) + 2 : 6);
    BEZEL_EXPECT_EQ(run(prog(
        "  static method Rep(p: string?, i: int): int "
        "    if i mod 2 = 0 then p := \"w\" + i.ToString(); end; "
        "    var s := $\"[{p}]\"; "
        "    return s.Length; "
        "  end Rep; ",
        "      var q: string? := null; "
        "      var s := $\"<{q}>\"; "
        "      t := t + s.Length + Rep(null, i); ")),
        std::to_string(want) + "\n");
}

// Three holes reading the same slot.
BEZEL_TEST(codegen_string_optional_interpolation_hole_5277,
           multiple_holes_same_slot_spec_18_5_28_9) {
    int want = 0;
    for (int i = 0; i < kIters; ++i) want += 3 * tagLen(i) + 2;
    BEZEL_EXPECT_EQ(run(prog("",
        "      var p: string? := \"w\" + i.ToString(); "
        "      var s := $\"{p}-{p}+{p}\"; "
        "      t := t + s.Length; ")), std::to_string(want) + "\n");
}

// The hole is evaluated repeatedly in an inner loop over one slot.
BEZEL_TEST(codegen_string_optional_interpolation_hole_5277,
           hole_in_loop_spec_18_5_28_9) {
    int want = 0;
    for (int i = 0; i < kIters; ++i) want += 3 * (tagLen(i) + 1);
    BEZEL_EXPECT_EQ(run(prog("",
        "      var p: string? := \"w\" + i.ToString(); "
        "      for var k in 0..<3 do "
        "        var s := $\"{p}{k}\"; "
        "        t := t + s.Length; "
        "      end; ")), std::to_string(want) + "\n");
}

// A `string?` auto-property hole, read directly and through a parameter that
// is not assigned. Pre-fix: heap-use-after-free (the hole released the
// field's string while the object still held it).
BEZEL_TEST(codegen_string_optional_interpolation_hole_5277,
           field_and_borrowed_param_hole_spec_18_5_28_9) {
    int want = 0;
    for (int i = 0; i < kIters; ++i) want += 2 * (tagLen(i) + 2);
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class C "
        "  public property N: string?; "
        "  public constructor() end constructor; "
        "end C; "
        "public class Program "
        "  static method Show(p: string?): int "
        "    var s := $\"{p}-y\"; "
        "    return s.Length; "
        "  end Show; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var t := 0; "
        "    for var i in 0..<200 do "
        "      c.N := \"f\" + i.ToString(); "
        "      var s := $\"{c.N}-x\"; "
        "      t := t + s.Length + Show(c.N); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;"), std::to_string(want) + "\n");
}

// Guard (passes before and after): a `string?`-returning call is a +1
// producer, so its hole must still be released (no leak), null or not.
BEZEL_TEST(codegen_string_optional_interpolation_hole_5277,
           producer_call_hole_still_released_spec_18_5_28_9) {
    int want = 0;
    for (int i = 0; i < kIters; ++i)
        want += (i % 3 == 0 ? 4 : tagLen(i)) + 2;
    BEZEL_EXPECT_EQ(run(prog(
        "  static method Mk(i: int): string? "
        "    if i mod 3 = 0 then return null; end; "
        "    return \"m\" + i.ToString(); "
        "  end Mk; ",
        "      var s := $\"{Mk(i)}-x\"; "
        "      t := t + s.Length; ")), std::to_string(want) + "\n");
}
