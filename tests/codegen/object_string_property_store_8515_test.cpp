// Codegen — a string stored into an `Object`-typed PROPERTY through an
// `Object`-typed value was stored without a retain while the string's owner
// still released it: use-after-free. §28.9 (Runtime-managed types), §8.6 (a
// reference type is not boxed), §17.4, §17.8, v0.357.
//
// THE DEFECT (issue #8515, a #8307 residual). The string-escape walk's
// property-SET relaxation (#2505 / #2535) reads the stored value as a safe
// read because "the setter retains" — true of a `string`-typed property. An
// `Object`/interface-typed property has no retaining setter: its `value` is
// an `Object`, stored raw (no runtime discriminator to retain through). #8307
// closed exactly this for the `Object` FIELD store (the value is safe only
// when it is statically a `string`, which `lowerObjectStringFieldStore`
// retains); the property twin still read every value as safe, so
// `var o: Object := Make.Str(i); h.P := o;` released `o` at scope end with
// the property still naming the string.
//
// MEASURED on the pre-fix build (r30-c1 0cb32bb14), `--sanitize=address`,
// both issue shapes below: heap-use-after-free.
//
// THE FIX. An `Object`/interface-typed property-SET (and the user-indexer
// twin) no longer takes the "setter retains" relaxation; it falls to the
// #8214 / #8307 arm, which reads the value as safe only for an object-string
// storage member (an auto-property is one) receiving a statically-`string`
// value. Every other store keeps the string's reference: the documented
// residual LEAK behind an `Object` erasure (#5059, #8459), never a dangling
// read. A `string`-typed property is untouched (the CONTROLS, leak-checked).
//
// The heap-store rows run on `runAotCheckedLenient` (ASan armed, LSan off);
// the controls on `runAotChecked`. Every row asserts
// `!checkedToolchainSkipped(r)` before any guard that could absorb the skip.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The defect is a REFCOUNT defect: one iteration can hide it, 100 cannot.
constexpr int kIterations = 100;

// Sum over i in 0..99 of ("s" + i).Length: 10 * 2 + 90 * 3.
const char* kLengthTotal = "290\n";

// The issue's Holder (`public property P: Object := "";`), with the accessor
// twins: a block property `Q` over a backing field, an `Object` indexer, an
// interface property `IP`, and a `string` auto-property `PS` for the controls.
const char* kDecls =
    "public interface IHold "
    "  property IP: Object; "
    "end IHold; "
    "public class Holder : IHold "
    "  public property P: Object := \"\"; "
    "  public property PS: string := \"\"; "
    "  public property IP: Object := \"\"; "
    "  public _q: Object := \"\"; "
    "  public property Q: Object "
    "    get => _q; "
    "    set => do _q := value; end; "
    "  end; "
    "  public _a: Object := \"\"; "
    "  public property [i: int]: Object "
    "    get => _a; "
    "    set => do _a := value; end; "
    "  end; "
    "end Holder; "
    "public class Make "
    "  public static method Str(n: int): string => \"s\" + n.ToString(); "
    "end Make; ";

// `body` runs once per iteration with `h` (a fresh Holder), `i` and `total`
// in scope; the program prints `total`.
std::string program(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program "
           "  public static method Main(): int "
           "    var i := 0; var total := 0; "
           "    while i < " + std::to_string(kIterations) + " do "
           "      var h := new Holder(); "
           + body +
           "      i := i + 1; "
           "    end; "
           "    Console.WriteLine(total); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// A heap string is stored by the body's `do ... end` block, whose end
// releases the string's owner; `read` (an int expression over `h`) reads the
// stored value back afterwards, after a fresh allocation that would reuse a
// freed block.
std::string storeThenRead(const std::string& store, const std::string& read) {
    return program("      do " + store + " end; "
                   "      var junk := Make.Str(99999); "
                   "      total := total + " + read + " + "
                   "junk.Length - junk.Length; ");
}

const char* kReadP = "h.P.ToString().Length";
const char* kReadQ = "h.Q.ToString().Length";
const char* kReadIdx = "h[0].ToString().Length";
const char* kReadIP = "h.IP.ToString().Length";

// ASan armed, LSan off: ran, exit 0, exact stdout. A use-after-free, double
// free or over-release fails the row; the slot's documented +1 leak does not.
void expectNoUseAfterFree(const std::string& src, const std::string& expected) {
    JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// ASan AND LSan armed: ran, exit 0, exact stdout, nothing leaked.
void expectCleanRun(const std::string& src, const std::string& expected) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN SHAPES (#8515).
// ===========================================================================

// y06 / f04: an `Object` local holding a +1 producer, stored into the `Object`
// auto-property.
BEZEL_TEST(object_string_property_store_8515,
           issue_y06_object_local_into_object_auto_property_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); h.P := o;", kReadP),
        kLengthTotal);
}

// y05 / f05: `(x as Object)!` stored into the `Object` auto-property.
BEZEL_TEST(object_string_property_store_8515,
           issue_y05_as_object_bang_into_object_auto_property_spec_28_9_17_8) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); h.P := (x as Object)!;", kReadP),
        kLengthTotal);
}

// ===========================================================================
// NEIGHBOURS — the same store through the other `Object`-typed sources and
// the other `Object`-typed accessors.
// ===========================================================================

// An `as Object` alias local (retained at bind, #8307) stored into the
// property: the alias's reference must not be released at scope end.
BEZEL_TEST(object_string_property_store_8515,
           as_object_alias_local_into_object_auto_property_spec_28_9_17_4) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); var o := x as Object; h.P := o;",
                      kReadP),
        kLengthTotal);
}

// A `!`-bound `Object` alias (#8514) stored into the property.
BEZEL_TEST(object_string_property_store_8515,
           bang_object_alias_into_object_auto_property_spec_28_9_17_8) {
    expectNoUseAfterFree(
        storeThenRead("var x: string? := Make.Str(i); var o: Object := x!; "
                      "h.P := o;",
                      kReadP),
        kLengthTotal);
}

// The same value stored twice.
BEZEL_TEST(object_string_property_store_8515,
           object_local_into_object_auto_property_twice_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); h.P := o; h.P := o;",
                      kReadP),
        kLengthTotal);
}

// A block-form `Object` property: the setter's `value` is an `Object` formal,
// stored raw into the backing field.
BEZEL_TEST(object_string_property_store_8515,
           object_local_into_object_block_property_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); h.Q := o;", kReadQ),
        kLengthTotal);
}
BEZEL_TEST(object_string_property_store_8515,
           string_local_into_object_block_property_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); h.Q := x;", kReadQ),
        kLengthTotal);
}

// The user-indexer twin.
BEZEL_TEST(object_string_property_store_8515,
           object_local_into_object_indexer_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); h[0] := o;", kReadIdx),
        kLengthTotal);
}
BEZEL_TEST(object_string_property_store_8515,
           string_local_into_object_indexer_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); h[0] := x;", kReadIdx),
        kLengthTotal);
}

// The property reached through an interface-typed receiver.
BEZEL_TEST(object_string_property_store_8515,
           object_local_into_interface_object_property_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var ih: IHold := h; var o: Object := Make.Str(i); "
                      "ih.IP := o;",
                      kReadIP),
        kLengthTotal);
}
BEZEL_TEST(object_string_property_store_8515,
           string_local_into_interface_object_property_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var ih: IHold := h; var x := Make.Str(i); ih.IP := x;",
                      kReadIP),
        kLengthTotal);
}

// ===========================================================================
// CONTROLS — leak-checked. A `string`-typed property's setter retains, so its
// relaxation is unchanged.
// ===========================================================================

// BOUNDARY CONTROL (base-green, kept: leak check). Goes RED under the wrong
// fix `drop the setter-retains relaxation for every auto-property`, not only
// the Object-typed ones (a string-typed store would then leak).
BEZEL_TEST(object_string_property_store_8515,
           control_string_local_into_string_auto_property_spec_28_9) {
    expectCleanRun(
        storeThenRead("var x := Make.Str(i); h.PS := x;", "h.PS.Length"),
        kLengthTotal);
}
// BOUNDARY CONTROL (base-green, kept: leak check). Goes RED under the wrong
// fix `key the tightening on the initializer's Object type instead of the
// target property's type` (`o as string` stored into a string property).
BEZEL_TEST(object_string_property_store_8515,
           control_object_holder_as_string_into_string_property_spec_28_9_17_4) {
    expectCleanRun(
        storeThenRead("var o: Object := Make.Str(i); h.PS := (o as string)!;",
                      "h.PS.Length"),
        kLengthTotal);
}
