// fix #8223 (stdlib §5.1, spec §11.8 / §14.2 *Interface conformance*, v0.355):
// the no-selector `Sum(): owned T` over a USER `INumber<T>` record conformer.
//
// THE DEFECT. stdlib §5.1 declares `extension IEnumerable<T> where T:
// INumber<T> { public method Sum(): owned T; }` -- an accumulation over
// `T.Zero()` and `T`'s `+` (spec §11.8's own `Sum<T>` example). The codegen
// `Sum` arm folded with a primitive `add`, so a record element reached
// `add i32 %.., %record.Money` and `bezelc` aborted with "IR verification
// failed (malformed IR emitted)". MEASURED on the parent (r30-c1 @ 3774bbfa):
// every generic row below (direct conformer, interface-extending conformer,
// method-level and class-level generic) failed to compile.
//
// The seed is `T.Zero()` (stdlib §5.1; stdlib §5 *empty source*: "`Sum` ...
// returns the zero of its result type (`U.Zero()`/`T.Zero()`)"), so an empty
// array yields the conformer's Zero, here Cents == 0.
//
// Controls: `Array<int>.Sum()` and `Array<double>.Sum()` keep the primitive
// fold (the eleven primitive conformers, stdlib §2.19).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A record conformer to `iface<Money>` carrying a `string` Label (a
// runtime-managed field). `+` keeps the LEFT operand's Label, so the Label of
// a sum is the SEED's ("z", from Zero) -- which pins that the fold starts at
// `T.Zero()` and not at the first element.
std::string money(const char* iface) {
    return std::string("public record Money(Cents: int, Label: string): ") +
           iface + "<Money>\n"
           "  public static method Zero(): Money => new Money(0, \"z\");\n"
           "  public static method One(): Money => new Money(1, \"o\");\n"
           "  public static method operator +(a: Money, b: Money): Money =>"
           " new Money(a.Cents + b.Cents, a.Label);\n"
           "  public static method operator -(a: Money, b: Money): Money =>"
           " new Money(a.Cents - b.Cents, a.Label);\n"
           "  public static method operator *(a: Money, b: Money): Money =>"
           " new Money(a.Cents * b.Cents, a.Label);\n"
           "  public static method operator /(a: Money, b: Money): Money =>"
           " new Money(a.Cents / b.Cents, a.Label);\n"
           "  public static method operator <(a: Money, b: Money): bool =>"
           " a.Cents < b.Cents;\n"
           "  public static method operator >(a: Money, b: Money): bool =>"
           " a.Cents > b.Cents;\n"
           "  public static method operator <=(a: Money, b: Money): bool =>"
           " a.Cents <= b.Cents;\n"
           "  public static method operator >=(a: Money, b: Money): bool =>"
           " a.Cents >= b.Cents;\n"
           "end Money;\n";
}

const char* kIface =
    "public interface IMyNum<T>: INumber<T> where T: IMyNum<T>\n"
    "end IMyNum;\n";

// `body` is the body of `Main`; the harness class holds the generic carriers.
std::string program(const char* iface, const std::string& body) {
    return std::string("module M;\n") + kIface + money(iface) +
           "public class Box<T> where T: INumber<T>\n"
           "  public method Total(a: Array<T>): T => a.Sum();\n"
           "end Box;\n"
           "public class Program\n"
           "  static method H<T>(a: Array<T>): T where T: INumber<T> =>"
           " a.Sum();\n"
           "  static method G<T>(a: Array<T>): T where T: IMyNum<T> =>"
           " a.Sum();\n"
           "  public static method Main()\n" +
           body +
           "  end Main;\n"
           "end Program;\n";
}

const char* kFill =
    "    var a := new Array<Money>();\n"
    "    a.Add(new Money(5, \"a\")); a.Add(new Money(6, \"b\"));\n";

}  // namespace

// Direct conformer (`: INumber<Money>`), method-level generic.
BEZEL_TEST(codegen_linq_sum_user_inumber_8223, direct_conformer_method_generic) {
    JitResult r = runAotChecked(program(
        "INumber",
        std::string(kFill) +
            "    var s := H(a);\n"
            "    Console.WriteLine(s.Cents);\n"
            "    Console.WriteLine(s.Label);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}

// Interface-extending conformer (`IMyNum<T>: INumber<T>`), the #8198
// fix-forward's probe a14 shape.
BEZEL_TEST(codegen_linq_sum_user_inumber_8223,
           interface_extending_conformer_method_generic) {
    JitResult r = runAotChecked(program(
        "IMyNum",
        std::string(kFill) +
            "    var s := G(a);\n"
            "    Console.WriteLine(s.Cents);\n"
            "    Console.WriteLine(s.Label);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}

// Class-level `where T: INumber<T>` at T = Money.
BEZEL_TEST(codegen_linq_sum_user_inumber_8223, class_level_generic) {
    JitResult r = runAotChecked(program(
        "INumber",
        std::string(kFill) +
            "    var b := new Box<Money>();\n"
            "    Console.WriteLine(b.Total(a).Cents);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}

// Empty source: the result is the seed, `T.Zero()` (stdlib §5 empty-source
// rule, §5.1).
BEZEL_TEST(codegen_linq_sum_user_inumber_8223, empty_array_yields_zero_seed) {
    JitResult r = runAotChecked(program(
        "INumber",
        "    var a := new Array<Money>();\n"
        "    var s := H(a);\n"
        "    Console.WriteLine(s.Cents);\n"
        "    Console.WriteLine(s.Label);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\nz\n");
}

// A single element: Zero + x (the fold starts at the seed, not at the first
// element) -- the Label "z" prefix pins the seed.
BEZEL_TEST(codegen_linq_sum_user_inumber_8223, single_element_folds_from_seed) {
    JitResult r = runAotChecked(program(
        "INumber",
        "    var a := new Array<Money>();\n"
        "    a.Add(new Money(7, \"x\"));\n"
        "    var s := H(a);\n"
        "    Console.WriteLine(s.Cents);\n"
        "    Console.WriteLine(s.Label);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\nz\n");
}

// 100 iterations over a record holding a runtime-managed `string`: the
// accumulator replaced on every step must not leak (runAotChecked = ASan +
// LeakSanitizer), and the data must still be read.
BEZEL_TEST(codegen_linq_sum_user_inumber_8223, loop_100x_no_leak) {
    JitResult r = runAotChecked(program(
        "INumber",
        std::string(kFill) +
            "    var total := 0;\n"
            "    var i := 0;\n"
            "    while i < 100 do\n"
            "      total := total + H(a).Cents;\n"
            "      i := i + 1;\n"
            "    end while;\n"
            "    Console.WriteLine(total);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1100\n");
}

// CONTROLS: the primitive conformers keep the primitive fold.
BEZEL_TEST(codegen_linq_sum_user_inumber_8223, control_int_and_double_unchanged) {
    JitResult r = runAotChecked(
        "module M;\n"
        "public class Program\n"
        "  static method H<T>(a: Array<T>): T where T: INumber<T> =>"
        " a.Sum();\n"
        "  public static method Main()\n"
        "    var i := new Array<int>(); i.Add(5); i.Add(6);\n"
        "    var d := new Array<double>(); d.Add(1.5); d.Add(2.25);\n"
        "    Console.WriteLine(i.Sum());\n"
        "    Console.WriteLine(d.Sum());\n"
        "    Console.WriteLine(H(i));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n3.75\n11\n");
}
