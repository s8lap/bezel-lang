// Codegen / float and double keys of GroupBy, Distinct and ToSet — issue #6657.
// Spec: language spec §4.3.1 (v0.339): `NaN = NaN` is true and `-0.0 = +0.0`
// is true, so floating-point `=` is reflexive; stdlib §5 GroupBy / Distinct /
// ToSet group, dedup and collect by the key type's value equality
// (Bezel_Standard_Library.md, v0.339).
//
// THE BUG. bz_groupby_to_array kept a byte compare for float/double keys
// (#6532 carved them out), so 0.0 and -0.0, and NaNs of different payload,
// split into separate groups (5 groups for the 5 values below, expected 3).
// Distinct and ToSet over double/float did not compile at all: the key-equality
// trampoline (buildLinqTrampoline kind 3) fell through to an integer ICmp on a
// floating-point operand, a codegen IR-verification abort.
//
// Values (built with BitConverter, stdlib §2.12): 0.0, -0.0, NaN, a NaN with a
// different payload, 1.5. Expected: 3 groups, in first-occurrence order
// (0.0 x2, NaN x2, 1.5 x1).
//
// Oracle: runAot, stdout pinned byte-for-byte.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& T, const std::string& mk0,
                 const std::string& mkNegZero, const std::string& mkNan,
                 const std::string& mkNanP, const std::string& mkOne,
                 const std::string& body) {
    return "module M; import System.Collections.Linq; "
           "record R(x: double, n: int); "
           "class C "
           "  public static method Main(): int "
           "    using var a := new Array<" + T + ">(); "
           "    a.Add(" + mk0 + "); a.Add(" + mkNegZero + "); "
           "    a.Add(" + mkNan + "); a.Add(" + mkNanP + "); a.Add(" + mkOne + "); " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

std::string d(const std::string& bits) {
    return "BitConverter.Int64BitsToDouble(" + bits + ")";
}
std::string f(const std::string& bits) {
    return "BitConverter.Int32BitsToFloat(" + bits + ")";
}

const char* kGroups =
    "    for var g in a.GroupBy(lambda(v) => v) do "
    "      Console.WriteLine(\"g \" + g.Count().ToString()); "
    "    end; ";
const char* kDistinctSet =
    "    var dc := 0; for var v in a.Distinct() do dc := dc + 1; end; "
    "    using var ts := a.ToSet(); "
    "    Console.WriteLine(\"distinct \" + dc.ToString() + \" toset \" + ts.Count.ToString()); ";

std::string dbl(const char* body, const std::string& T = "double") {
    return prog(T, d("0L"), d("-9223372036854775808L"),
                d("9221120237041090560L"), d("9221120237041090851L"),
                d("4609434218613702656L"), body);
}

}  // namespace

BEZEL_TEST(float_key_reflexive_equality_6657, groupby_double_merges_zeros_and_nans_spec_4_3_1) {
    JitResult r = runAot(dbl(kGroups));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "g 2\ng 2\ng 1\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, groupby_float_merges_zeros_and_nans_spec_4_3_1) {
    JitResult r = runAot(prog("float", f("0"), f("-2147483648"), f("2143289344"),
                              f("2143289635"), f("1069547520"), kGroups));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "g 2\ng 2\ng 1\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, groupby_nullable_double_and_record_field_stay_merged) {
    JitResult r = runAot(dbl(kGroups, "double?"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "g 2\ng 2\ng 1\n");
    JitResult q = runAot(prog("R", "new R(" + d("0L") + ", 7)",
                              "new R(" + d("-9223372036854775808L") + ", 7)",
                              "new R(" + d("9221120237041090560L") + ", 7)",
                              "new R(" + d("9221120237041090851L") + ", 7)",
                              "new R(" + d("4609434218613702656L") + ", 7)", kGroups));
    BEZEL_EXPECT_TRUE(q.ran);
    BEZEL_EXPECT_EQ(q.stdoutText, "g 2\ng 2\ng 1\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, distinct_and_toset_double_compile_and_dedup_spec_4_3_1) {
    JitResult r = runAot(dbl(kDistinctSet));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "distinct 3 toset 3\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, distinct_and_toset_float_compile_and_dedup_spec_4_3_1) {
    JitResult r = runAot(prog("float", f("0"), f("-2147483648"), f("2143289344"),
                              f("2143289635"), f("1069547520"), kDistinctSet));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "distinct 3 toset 3\n");
}

// Fix-forward #6657 (merge with #6219): GroupBy's key-equality trampoline uses
// hashKeyRecordOfRef, as Distinct / ToSet do, so a body record with user
// Equals / GetHashCode / `=` groups by that equality (spec §25.2, §9.2, v0.339;
// float keys §4.3.1, v0.339). Without it GroupBy gave 4 groups instead of 2.

BEZEL_TEST(float_key_reflexive_equality_6657, groupby_body_record_user_equality_groups_with_distinct_toset_spec_25_2) {
    JitResult r = runAot(R"BZ(module P;
import System.Collections.Linq;
public record B
  public readonly property X: int;
  public constructor(x: int)
    X := x;
  end constructor;
  public method Equals(other: B): bool => (X mod 10) = (other.X mod 10);
  public method GetHashCode(): int => (X mod 10) + 500;
  public static method operator =(a: B, b: B): bool =>
    (a.X mod 10) = (b.X mod 10);
  public static method operator <>(a: B, b: B): bool =>
    (a.X mod 10) <> (b.X mod 10);
end B;
public class Program
  public static method Main()
    using var a := new Array<B>();
    a.Add(new B(5)); a.Add(new B(15)); a.Add(new B(6)); a.Add(new B(25));
    var gc := 0; var first := 0;
    for var g in a.GroupBy(lambda(v) => v) do
      if gc = 0 then first := g.Key.X; end;
      Console.WriteLine("group key " + g.Key.X.ToString() + " n=" + g.Count().ToString());
      gc := gc + 1;
    end;
    var dc := 0; for var v in a.Distinct() do dc := dc + 1; end;
    using var ts := a.ToSet();
    Console.WriteLine("groups " + gc.ToString() + " distinct " + dc.ToString() + " toset " + ts.Count.ToString());
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "group key 5 n=3\ngroup key 6 n=1\ngroups 2 distinct 2 toset 2\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, groupby_positional_record_user_equality_groups_with_distinct_toset_spec_25_2) {
    JitResult r = runAot(R"BZ(module P;
import System.Collections.Linq;
public record B(X: int)
  public method Equals(other: B): bool => (X mod 10) = (other.X mod 10);
  public method GetHashCode(): int => (X mod 10) + 500;
  public static method operator =(a: B, b: B): bool =>
    (a.X mod 10) = (b.X mod 10);
  public static method operator <>(a: B, b: B): bool =>
    (a.X mod 10) <> (b.X mod 10);
end B;
public class Program
  public static method Main()
    using var a := new Array<B>();
    a.Add(new B(5)); a.Add(new B(15)); a.Add(new B(6)); a.Add(new B(25));
    var gc := 0; var first := 0;
    for var g in a.GroupBy(lambda(v) => v) do
      if gc = 0 then first := g.Key.X; end;
      Console.WriteLine("group key " + g.Key.X.ToString() + " n=" + g.Count().ToString());
      gc := gc + 1;
    end;
    var dc := 0; for var v in a.Distinct() do dc := dc + 1; end;
    using var ts := a.ToSet();
    Console.WriteLine("groups " + gc.ToString() + " distinct " + dc.ToString() + " toset " + ts.Count.ToString());
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "group key 5 n=3\ngroup key 6 n=1\ngroups 2 distinct 2 toset 2\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, union_intersect_except_double_reflexive_spec_4_3_1) {
    JitResult r = runAot(R"BZ(module M;
import System.Collections.Linq;
class C
  public static method Main(): int
    using var a := new Array<double>();
    a.Add(BitConverter.Int64BitsToDouble(0L)); a.Add(BitConverter.Int64BitsToDouble(9221120237041090560L)); a.Add(1.5);
    using var b := new Array<double>();
    b.Add(BitConverter.Int64BitsToDouble(-9223372036854775808L)); b.Add(BitConverter.Int64BitsToDouble(9221120237041090851L)); b.Add(2.5);
    var u := 0; for var v in a.Union(b) do u := u + 1; end;
    var i := 0; for var v in a.Intersect(b) do i := i + 1; end;
    var e := 0; for var v in a.Except(b) do e := e + 1; end;
    Console.WriteLine("Union " + u.ToString() + " Intersect " + i.ToString() + " Except " + e.ToString());
    return 0;
  end Main;
end C;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Union 4 Intersect 2 Except 1\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, union_intersect_except_float_reflexive_spec_4_3_1) {
    JitResult r = runAot(R"BZ(module M;
import System.Collections.Linq;
class C
  public static method Main(): int
    using var a := new Array<float>();
    a.Add(BitConverter.Int32BitsToFloat(0)); a.Add(BitConverter.Int32BitsToFloat(2143289344)); a.Add(BitConverter.Int32BitsToFloat(1069547520));
    using var b := new Array<float>();
    b.Add(BitConverter.Int32BitsToFloat(-2147483648)); b.Add(BitConverter.Int32BitsToFloat(2143289635)); b.Add(BitConverter.Int32BitsToFloat(1075838976));
    var u := 0; for var v in a.Union(b) do u := u + 1; end;
    var i := 0; for var v in a.Intersect(b) do i := i + 1; end;
    var e := 0; for var v in a.Except(b) do e := e + 1; end;
    Console.WriteLine("Union " + u.ToString() + " Intersect " + i.ToString() + " Except " + e.ToString());
    return 0;
  end Main;
end C;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Union 4 Intersect 2 Except 1\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, first_occurrence_representative_key_double_spec_4_3_1) {
    JitResult r = runAot(R"BZ(module M;
import System.Collections.Linq;
record R(x: double, n: int);
class C
  public static method Main(): int
    using var a := new Array<double>();
    a.Add(BitConverter.Int64BitsToDouble(-9223372036854775808L));
    a.Add(BitConverter.Int64BitsToDouble(0L));
    a.Add(BitConverter.Int64BitsToDouble(9221120237041090851L));
    a.Add(BitConverter.Int64BitsToDouble(9221120237041090560L));
    a.Add(BitConverter.Int64BitsToDouble(4609434218613702656L));
    a.Add(BitConverter.Int64BitsToDouble(0L));
    for var g in a.GroupBy(lambda(v) => v) do
      Console.WriteLine("GroupBy key=" + (BitConverter.DoubleToInt64Bits(g.Key)).ToString() + " n=" + g.Count().ToString());
    end;
    for var v in a.Distinct() do
      Console.WriteLine("Distinct " + (BitConverter.DoubleToInt64Bits(v)).ToString());
    end;
    using var ts := a.ToSet();
    Console.WriteLine("ToSet count " + ts.Count.ToString());
    for var v in ts do
      Console.WriteLine("ToSet " + (BitConverter.DoubleToInt64Bits(v)).ToString());
    end;
    return 0;
  end Main;
end C;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "GroupBy key=-9223372036854775808 n=3\nGroupBy key=9221120237041090851 n=2\nGroupBy key=4609434218613702656 n=1\nDistinct -9223372036854775808\nDistinct 9221120237041090851\nDistinct 4609434218613702656\nToSet count 3\nToSet -9223372036854775808\nToSet 9221120237041090851\nToSet 4609434218613702656\n");
}
