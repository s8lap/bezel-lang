// fix #8691 (stdlib §5 / §2.19, spec §11.8, §14.2 *Interface conformance*,
// v0.359): `Sum(selector)` whose selector returns a user `INumber<Self>`
// record is typed AND folded as that record.
//
// SPEC BASIS (QUOTED, v0.359).
//   stdlib §5: "public method Sum<U>(selector: (T) -> U): owned U where U:
//     INumber<U>;" -- `U` = the record the selector returns.
//   spec §11.8 / stdlib §5.1: the fold is `acc := U.Zero(); acc := acc + el`,
//     the conformer's own static members.
//
// THE DEFECT. The selector form of `Sum` was typed `int` for a record `U`
// ("no member `Cents` on primitive type `int`"); once typed, codegen still
// lowered the fold with the primitive accumulator, so `s.Label` crashed.
// #8223 fixed the NO-SELECTOR form only.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kMoney =
    "module M;\n"
    "public record Money(Cents: int, Label: string): INumber<Money>\n"
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
    "end Money;\n"
    "public record Item(Price: Money);\n";

std::string program(const char* mainBody) {
    return std::string(kMoney) +
           "public class Program\n"
           "  public static method Main()\n"
           "    var a := new Array<Item>();\n"
           "    a.Add(new Item(new Money(5, \"a\")));"
           " a.Add(new Item(new Money(6, \"b\")));\n" +
           mainBody +
           "  end Main;\n"
           "end Program;\n";
}

void expectPrints(const JitResult& r, const char* out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// The issue's repro verbatim. PARENT: "no member `Cents` on primitive type
// `int`".
BEZEL_TEST(codegen_linq_selector_record_sum_8691,
           issue_repro_record_selector_sum_is_the_record_spec_11_8) {
    expectPrints(runAot(program("    var s := a.Sum(lambda(i) => i.Price);\n"
                                "    Console.WriteLine(s.Cents);\n")),
                 "11\n");
}

// The fold's accumulator is the record, not an `int` slot: the `string` member
// of the folded value is the conformer's own (`Zero().Label`). PARENT: rejected.
BEZEL_TEST(codegen_linq_selector_record_sum_8691,
           record_selector_sum_folds_with_the_conformers_zero_and_plus) {
    expectPrints(runAot(program("    var s := a.Sum(lambda(i) => i.Price);\n"
                                "    Console.WriteLine(s.Label);\n")),
                 "z\n");
}

// The lazy receiver arm. PARENT: rejected.
BEZEL_TEST(codegen_linq_selector_record_sum_8691,
           record_selector_sum_lazy_receiver_spec_11_8) {
    expectPrints(
        runAot(program(
            "    var t := a.Where(lambda(i) => i.Price.Cents > 5)"
            ".Sum(lambda(i) => i.Price);\n"
            "    Console.WriteLine(t.Cents);\n")),
        "6\n");
}

// Both oracles, ownership-sensitive: a record carrying a `string` folds
// without a leak or a double release.
BEZEL_TEST(codegen_linq_selector_record_sum_8691,
           record_selector_sum_is_clean_under_the_checked_oracle) {
    JitResult r = runAotChecked(
        program("    var s := a.Sum(lambda(i) => i.Price);\n"
                "    Console.WriteLine(s.Cents);\n"
                "    Console.WriteLine(s.Label);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}
