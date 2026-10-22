// Spec-derived regression test for issue #6998.
//
// §16.3 (v0.340): "`Range<T>` is a compiler-intrinsic record that implements
// `IEnumerable<T>`". §5.1 (v0.340): a type implicitly converts "to an interface
// it implements".
//
// THE DEFECT. A `Range<int>` (literal `1..4` or a local) passed to an
// `IEnumerable<int>` parameter was rejected, "not implicitly convertible
// (§5.1)": the builtin Range symbol carries no base list, so the user-type
// conformance walk found no `IEnumerable`. Admitting the conversion in sema is
// not enough: the lowered range is a by-value `%bz.range` struct with no
// vtable, so codegen has to mint an `IEnumerable<T>` wrapper at the three
// coercion sites (argument, local-decl init, RETURN) or the IR is malformed.
//
// Each row prints exact stdout. The RUN rows use runAotChecked (leak + ASan
// oracle); the 100x loop row would leak the drained array per iteration if the
// wrapper's disposal were not registered.
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kHelpers =
    "public class H "
    "  public static method Sum(xs: IEnumerable<int>): int "
    "    var n := 0; for var x in xs do n += x; end; return n; "
    "  end Sum; "
    "  public static method SumL(xs: IEnumerable<long>): long "
    "    var n: long := 0; for var x in xs do n += x; end; return n; "
    "  end SumL; "
    "  public static method Chars(xs: IEnumerable<char>): string "
    "    var s := \"\"; for var c in xs do s := s + $\"{c}\"; end; return s; "
    "  end Chars; "
    "  public static method Seq(xs: IEnumerable<int>): string "
    "    var s := \"\"; for var x in xs do s := s + $\"{x},\"; end; return s; "
    "  end Seq; "
    "  public static method Make(): IEnumerable<int> return 1..3; end Make; "
    "  public static method MakeLocal(): IEnumerable<int> "
    "    var r: Range<int> := 10..12; return r; "
    "  end MakeLocal; "
    "end H; ";

JitResult run(const char* mod, const std::string& body) {
    return runAotChecked(std::string("module ") + mod + "; " + kHelpers +
                         "public class M public static method Main(): int " +
                         body + " return 0; end Main; end M;");
}

}  // namespace

#define ROW(NAME, MOD, BODY, EXPECT)                                          \
    BEZEL_TEST(codegen_range_to_ienumerable_6998, NAME) {                     \
        JitResult r = run(MOD, BODY);                                         \
        BEZEL_EXPECT_TRUE(r.ran);                                             \
        BEZEL_EXPECT_EQ(r.exitCode, 0);                                       \
        BEZEL_EXPECT_EQ(r.stdoutText, std::string(EXPECT));                   \
    }

ROW(literal_range_argument_spec_16_3, "M6998a",
    "Console.WriteLine($\"{H.Sum(1..4)}\");", "10\n")
ROW(local_range_argument_spec_16_3, "M6998b",
    "var r: Range<int> := 1..4; Console.WriteLine($\"{H.Sum(r)}\");", "10\n")
ROW(half_open_range_spec_16_3, "M6998c",
    "Console.WriteLine(H.Seq(1..<4));", "1,2,3,\n")
ROW(empty_range_spec_16_3, "M6998d",
    "Console.WriteLine($\"{H.Sum(5..<5)}\");", "0\n")
ROW(descending_range_spec_16_3, "M6998e",
    "Console.WriteLine(H.Seq(4..1 step -1));", "4,3,2,1,\n")
ROW(stepped_range_spec_16_3, "M6998f",
    "Console.WriteLine(H.Seq(0..10 step 5));", "0,5,10,\n")
ROW(long_range_spec_16_3, "M6998g",
    "var r: Range<long> := 1L..3L; Console.WriteLine($\"{H.SumL(r)}\");", "6\n")
ROW(char_range_spec_16_3, "M6998h",
    "Console.WriteLine(H.Chars('a'..'d'));", "abcd\n")
ROW(interface_local_spec_5_1, "M6998i",
    "var e: IEnumerable<int> := 5..7; var t := 0; "
    "for var x in e do t += x; end; Console.WriteLine($\"{t}\");", "18\n")
ROW(return_range_as_ienumerable_spec_5_1, "M6998j",
    "var t := 0; for var x in H.Make() do t += x; end; "
    "for var x in H.MakeLocal() do t += x; end; Console.WriteLine($\"{t}\");",
    "39\n")
ROW(linq_over_interface_value_spec_16_3, "M6998k",
    "var e: IEnumerable<int> := 1..10; "
    "Console.WriteLine($\"{e.Where(lambda(x) => x mod 2 = 0).Count()}\");",
    "5\n")
ROW(range_local_reused_across_calls_spec_5_1, "M6998l",
    "var r: Range<int> := 1..3; "
    "Console.WriteLine($\"{H.Sum(r)} {H.Sum(r)} {r.Start}\");", "6 6 1\n")
ROW(loop_100x_spec_16_3, "M6998n",
    "var t := 0; var i := 0; while i < 100 do t += H.Sum(1..3); i += 1; end; "
    "Console.WriteLine($\"{t}\");", "600\n")
ROW(interface_local_in_loop_spec_5_1, "M6998o",
    "var t := 0; for var i in 0..<100 do "
    "var e: IEnumerable<int> := 1..3; for var x in e do t += x; end; end; "
    "Console.WriteLine($\"{t}\");", "600\n")

BEZEL_TEST(codegen_range_to_ienumerable_6998,
           mismatched_element_type_still_rejected_spec_5_1) {
    JitResult r = runAot(
        std::string("module M6998p; ") + kHelpers +
        "public class M public static method Main(): int "
        "Console.WriteLine($\"{H.SumL(1..4)}\"); return 0; "
        "end Main; end M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "not implicitly convertible"));
}

BEZEL_TEST(codegen_range_to_ienumerable_6998,
           range_int_to_ienumerable_string_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M6998q; "
        "public class M public static method Main(): int "
        "var e: IEnumerable<string> := 1..4; return 0; "
        "end Main; end M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "not implicitly convertible"));
}

// The control: an `Array<int>` argument to the same parameter shape. It passes
// on the pre-fix compiler too (it is the issue's `rng_arr` control), so it
// carries its own minimal program, free of the range-returning helpers.
BEZEL_TEST(codegen_range_to_ienumerable_6998, array_control_spec_5_1) {
    JitResult r = runAotChecked(
        "module M6998m; "
        "public class H public static method Sum(xs: IEnumerable<int>): int "
        "var n := 0; for var x in xs do n += x; end; return n; "
        "end Sum; end H; "
        "public class M public static method Main(): int "
        "var a := new Array<int>(0); a.Add(4); a.Add(5); "
        "Console.WriteLine($\"{H.Sum(a)}\"); a.Dispose(); return 0; "
        "end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n"));
}
