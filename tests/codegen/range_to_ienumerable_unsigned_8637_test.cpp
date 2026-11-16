// Regression test for issue #8637 — a narrow/unsigned range wrapped as
// `IEnumerable<T>` drained EMPTY when its span crossed the signed midpoint.
//
// §16.3 (v0.357): "`Range<T>` is a compiler-intrinsic record that implements
// `IEnumerable<T>`", and its `Start` / `End` are `T?` — T VALUES, with `T`
// restricted to the integer types and `char`. The same section (v0.357):
// "`Step` is a signed 64-bit integer, to support descending iteration over
// both signed and unsigned `T`", and "Iteration stops silently, raising no
// `OverflowException`, before the first value that would pass the range's
// endpoint or fall outside `T`'s representable range, whichever comes first"
// — and "`GetEnumerator` produces an enumerator that yields values from
// `Start` to `End` (inclusive or exclusive per `IsEndInclusive`), incrementing
// by `Step`", so the bounds are compared in T's ORDERING. §16.5 (v0.357): "The
// compiler may optimize `for var i in rangeExpr` do ... into a counted loop
// that bypasses the `IEnumerable` protocol, provided the observable behavior
// is identical" — so the lazy enumeration and the counted loop must agree.
//
// THE DEFECT (measured on the r30-c1 base, b770c3650). The `%bz.range` struct
// stores each endpoint in an i64 SIGN-EXTENDED (the struct's uniform
// convention), so an unsigned endpoint above the signed midpoint of its width
// (`byte 200`) is stored as -56. The lazy range source compared its bounds
// with a SIGNED predicate, so `byte 0..200` read as `0 .. -56` — an EMPTY
// range — and the range->`IEnumerable<T>` wrap yielded ZERO elements where the
// direct `for var x in a..b` counted 201. The same held for ushort, for uint
// across 2^31 and for ulong across 2^63. The signed widths were already
// correct; the `long` MIN/MAX range was worse than empty — the advance wrapped
// past long.MaxValue and the enumeration never ended.
//
// Each row prints the direct counted loop and the three `IEnumerable<T>` faces
// of the SAME range (a typed formal, an explicitly-typed generic call and the
// inferred generic call), and every face must agree.
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The issue's own two faces, under the issue's own names: `SumC(xs:
// IEnumerable<byte>)` (the typed formal) and `SumI<byte>(a..b)` (the generic
// face). The issue's repro is:
//
//   var a: byte := 0; var b: byte := 200;
//   Console.WriteLine(SumC(a..b));        // -> 0 (expected 201)
//   Console.WriteLine(SumI<byte>(a..b));  // -> 0 (expected 201)
std::string helpers(const std::string& ty) {
    return "public class H "
           "  public static method SumC(xs: IEnumerable<" + ty + ">): long "
           "    var n: long := 0; for var x in xs do n++; end; return n; "
           "  end SumC; "
           "  public static method SumI<T>(xs: IEnumerable<T>): long "
           "    var n: long := 0; for var x in xs do n++; end; return n; "
           "  end SumI; "
           "end H; ";
}

// One range case: the counted loop, then the typed / explicitly-typed /
// inferred `IEnumerable<T>` faces of the same range.
std::string rangeCase(const std::string& ty, const std::string& lo,
                      const std::string& hi, const std::string& step, int i) {
    const std::string n = std::to_string(i);
    std::string rng = "a" + n + "..b" + n;
    if (!step.empty()) rng += " step " + step;
    return "    var a" + n + ": " + ty + " := " + lo + "; "
           "var b" + n + ": " + ty + " := " + hi + ";\n"
           "    var f" + n + " := 0;\n"
           "    for var x in " + rng + " do f" + n + "++; end;\n"
           "    Console.WriteLine(f" + n + ");\n"
           "    Console.WriteLine(H.SumC(" + rng + "));\n"
           "    Console.WriteLine(H.SumI<" + ty + ">(" + rng + "));\n"
           "    Console.WriteLine(H.SumI(" + rng + "));\n";
}

JitResult run(const std::string& mod, const std::string& ty,
              const std::string& body) {
    return runAotChecked(std::string("module ") + mod + "; " + helpers(ty) +
                         "public class Program public static method Main(): int " +
                         body + " return 0; end Main; end Program;");
}

// The expected stdout of `count` faces of the same count.
std::string same(const std::string& n, int times) {
    std::string s;
    for (int i = 0; i < times; i++) s += n + "\n";
    return s;
}

}  // namespace

#define ROW(NAME, MOD, TY, BODY, EXPECT)                                      \
    BEZEL_TEST(codegen_range_to_ienumerable_unsigned_8637, NAME) {            \
        JitResult r = run(MOD, TY, BODY);                                      \
        BEZEL_EXPECT_TRUE(r.ran);                                             \
        BEZEL_EXPECT_EQ(r.exitCode, 0);                                       \
        BEZEL_EXPECT_EQ(r.stdoutText, std::string(EXPECT));                   \
    }

// §16.3: `byte 0..200` — the span crosses byte's signed midpoint (128), so
// the endpoint 200 is stored as -56 and the range read as empty.
ROW(byte_span_above_signed_midpoint_spec_16_3, "M8637byte", "byte",
    rangeCase("byte", "0", "200", "", 0) + rangeCase("byte", "200", "0", "-1", 1),
    same("201", 8))

// §16.3: byte's MIN/MAX endpoints with a step.
ROW(byte_min_max_endpoints_spec_16_3, "M8637byte2", "byte",
    rangeCase("byte", "0", "255", "50", 0),
    same("6", 4))

// §16.3: ushort 0..40000 — above ushort's signed midpoint (32767).
ROW(ushort_span_above_signed_midpoint_spec_16_3, "M8637ushort", "ushort",
    rangeCase("ushort", "0", "40000", "", 0) +
        rangeCase("ushort", "40000", "0", "-1", 1),
    same("40001", 8))

// §16.3: ushort's MIN/MAX endpoints with a step.
ROW(ushort_min_max_endpoints_spec_16_3, "M8637ushort2", "ushort",
    rangeCase("ushort", "0", "65535", "5000", 0),
    same("14", 4))

// §16.3: uint 0..3000000000 — above uint's signed midpoint (2^31 - 1).
ROW(uint_span_above_2_31_spec_16_3, "M8637uint", "uint",
    rangeCase("uint", "0U", "3000000000U", "1000000000U", 0) +
        rangeCase("uint", "3000000000U", "0U", "-1000000000", 1),
    same("4", 8))

// §16.3: uint's MIN/MAX endpoints with a step.
ROW(uint_min_max_endpoints_spec_16_3, "M8637uint2", "uint",
    rangeCase("uint", "0U", "4294967295U", "1000000000U", 0),
    same("5", 4))

// §16.3: ulong across 2^63 — the endpoint 9223372036854775809 is above long's
// signed maximum, so a signed reading of the bounds ordered the range
// backwards and it drained empty.
ROW(ulong_span_above_2_63_spec_16_3, "M8637ulong", "ulong",
    rangeCase("ulong", "9223372036854775806UL", "9223372036854775809UL", "", 0) +
        rangeCase("ulong", "0UL", "9223372036854775810UL",
                  "1000000000000000000UL", 1) +
        rangeCase("ulong", "9223372036854775810UL", "0UL",
                  "-1000000000000000000L", 2),
    same("4", 4) + same("10", 8))

// §16.3: ulong's MIN/MAX endpoints with a step (the whole 64-bit range).
ROW(ulong_min_max_endpoints_spec_16_3, "M8637ulong2", "ulong",
    rangeCase("ulong", "0UL", "18446744073709551615UL",
              "1000000000000000000UL", 0),
    same("19", 4))

// §16.3: a `long` range over the type's MIN/MAX endpoints. The iteration
// must stop before the first value that would leave `long`'s representable
// range; the advance used to wrap past long.MaxValue and the enumeration
// never ended. The `Take(25)` bounds the enumeration, so a compiler that
// never stops still terminates and is caught by the count.
ROW(long_min_max_endpoints_stop_at_type_range_spec_16_3, "M8637long", "long",
    "    var l0: long := -9223372036854775808L; "
    "var h0: long := 9223372036854775807L;\n"
    "    Console.WriteLine((l0..h0 step 1000000000000000000L).Take(25).Count());\n"
    "    var l1: long := -1000L; var h1: long := 1000L;\n"
    "    Console.WriteLine(H.SumC(l1..h1));\n",
    "19\n2001\n")
