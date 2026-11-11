// Codegen — a method whose declared return type is `Object` (or an interface)
// and which returns a borrowed `string` handed its caller the string's bare
// pointer with NO reference: use-after-free once the string's owner released
// it. §28.9 (Runtime-managed types), §8.6 (a reference type is not boxed),
// §17.4, §17.8, v0.357.
//
// THE DEFECT (issue #8537, a #8307 residual). A `string`-typed return MINTS:
// the callee retains a borrowed value (a parameter, a field) so the caller
// owns a reference. The same value returned through `Object` minted nothing,
// and the caller — which cannot tell an `Object` result is a string — stored
// it raw. `h.O := Conv.F(x);` therefore dangled as soon as `x`'s scope-end
// release ran, and `h.O := Conv.F(Make.Str(i));` as soon as the #429
// post-call release of the unbound producer argument ran.
//
// MEASURED on the pre-fix build (r30-c1 0cb32bb14), `--sanitize=address`,
// every heap-store row below: heap-use-after-free.
//
// THE FIX. An `Object`/interface-typed return of a statically-`string` value
// (under any run of `!` and pointer-preserving `as`) takes the reference the
// `string`-typed return takes, with the same exclusions (a +1 producer, a
// local that owns the reference it transfers) — in the block-bodied,
// expression-bodied, virtual, lambda and block-getter spellings.
//
// The caller of an `Object`-returning method cannot release the result, so
// the minted reference is never dropped: the documented residual LEAK of a
// string behind an `Object` erasure (#5059, #8459), never a dangling read.
// Every row therefore runs on `runAotCheckedLenient` (ASan armed, LSan off)
// except the controls that mint nothing (a literal, a `string`-typed
// return), which stay leak-checked on `runAotChecked`. Every row asserts
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

// The issue's `Conv2.F(x: string): Object` in both of its spellings (`return
// x;` / `return x as Object;`), plus the expression-bodied, `!`, conditional,
// virtual and field-source twins.
const char* kDecls =
    "public class Holder "
    "  public O: Object := \"\"; "
    "  public S: string := \"\"; "
    "  public method Put(o: Object) "
    "    this.O := o; "
    "  end Put; "
    "  public method PutRet(x: string) "
    "    this.O := Conv.F(x); "
    "  end PutRet; "
    "  public method GetS(): Object => this.S; "
    "  public method GetS2(): Object "
    "    return this.S; "
    "  end GetS2; "
    "end Holder; "
    "public class Conv "
    "  public static method F(x: string): Object "
    "    return x; "
    "  end F; "
    "  public static method FA(x: string): Object "
    "    return x as Object; "
    "  end FA; "
    "  public static method G(x: string): Object => x; "
    "  public static method GA(x: string): Object => x as Object; "
    "  public static method FB(x: string?): Object "
    "    return x!; "
    "  end FB; "
    "  public static method FC(x: string, c: bool): Object "
    "    if c then return x; end; "
    "    return \"zz\"; "
    "  end FC; "
    "  public static method SId(x: string): string => x; "
    "end Conv; "
    "public class VConv "
    "  public virtual method F(x: string): Object "
    "    return x; "
    "  end F; "
    "end VConv; "
    "public class Make "
    "  public static method Str(n: int): string => \"s\" + n.ToString(); "
    "end Make; ";

// `body` runs once per iteration with `h` (a fresh Holder), `vc` (a VConv),
// `f` / `fb` (the lambda twins), `i` and `total` in scope; the program prints
// `total`.
std::string program(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program "
           "  public static method Main(): int "
           "    var i := 0; var total := 0; "
           "    var vc := new VConv(); "
           "    var f := lambda(s: string): Object => s; "
           "    var fb := lambda(s: string): Object => do return s; end; "
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

// A heap string is stored into the field by the body's `do ... end` block,
// whose end releases the string's owner; the field is read back afterwards
// (after a fresh allocation that would reuse a freed block).
std::string storeThenRead(const std::string& store) {
    return program("      do " + store + " end; "
                   "      var junk := Make.Str(99999); "
                   "      total := total + h.O.ToString().Length + "
                   "junk.Length - junk.Length; ");
}

// ASan armed, LSan off: ran, exit 0, exact stdout. A use-after-free, double
// free or over-release fails the row; the documented +1 leak does not.
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
// THE ISSUE'S OWN SHAPES (#8537).
// ===========================================================================

// rw01: `return x;` from `F(x: string): Object`, called on a named local.
BEZEL_TEST(object_string_return_mint_8537,
           issue_rw01_return_string_param_named_local_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); h.O := Conv.F(x);"), kLengthTotal);
}

// rv24: `return x as Object;`, called on a named local.
BEZEL_TEST(object_string_return_mint_8537,
           issue_rv24_return_string_param_as_object_named_local_spec_28_9_17_4) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); h.O := Conv.FA(x);"), kLengthTotal);
}

// rw02: called on an unbound +1 producer (the #429 post-call release frees
// the argument).
BEZEL_TEST(object_string_return_mint_8537,
           issue_rw02_return_string_param_producer_argument_spec_28_9_8_6) {
    expectNoUseAfterFree(storeThenRead("h.O := Conv.F(Make.Str(i));"),
                         kLengthTotal);
}
BEZEL_TEST(object_string_return_mint_8537,
           issue_rw02_return_as_object_producer_argument_spec_28_9_17_4) {
    expectNoUseAfterFree(storeThenRead("h.O := Conv.FA(Make.Str(i));"),
                         kLengthTotal);
}

// ===========================================================================
// THE OTHER SPELLINGS of the same return.
// ===========================================================================

// Expression-bodied.
BEZEL_TEST(object_string_return_mint_8537,
           expression_bodied_return_string_param_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); h.O := Conv.G(x);"), kLengthTotal);
}
BEZEL_TEST(object_string_return_mint_8537,
           expression_bodied_return_as_object_producer_argument_spec_28_9_17_4) {
    expectNoUseAfterFree(storeThenRead("h.O := Conv.GA(Make.Str(i));"),
                         kLengthTotal);
}

// `return x!;` over a `string?` parameter.
BEZEL_TEST(object_string_return_mint_8537,
           return_bang_of_nullable_string_param_spec_28_9_17_8) {
    expectNoUseAfterFree(
        storeThenRead("var x: string? := Make.Str(i); h.O := Conv.FB(x);"),
        kLengthTotal);
}

// One of several returns.
BEZEL_TEST(object_string_return_mint_8537,
           conditional_return_string_param_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); h.O := Conv.FC(x, true);"),
        kLengthTotal);
}

// A virtual method (the caller cannot see the body).
BEZEL_TEST(object_string_return_mint_8537,
           virtual_return_string_param_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); h.O := vc.F(x);"), kLengthTotal);
}

// Lambdas whose result type is `Object`: expression-bodied and block-bodied.
BEZEL_TEST(object_string_return_mint_8537,
           lambda_expression_body_return_string_param_spec_28_9_8_6) {
    expectNoUseAfterFree(storeThenRead("var x := Make.Str(i); h.O := f(x);"),
                         kLengthTotal);
}
BEZEL_TEST(object_string_return_mint_8537,
           lambda_block_body_return_string_param_spec_28_9_8_6) {
    expectNoUseAfterFree(storeThenRead("var x := Make.Str(i); h.O := fb(x);"),
                         kLengthTotal);
}

// A FIELD source: the returned alias must survive the field being stored
// over (which releases the displaced string).
BEZEL_TEST(object_string_return_mint_8537,
           expression_bodied_return_string_field_then_overwritten_spec_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var k := new Holder(); k.S := Make.Str(i); "
                      "h.O := k.GetS(); k.S := \"zz\";"),
        kLengthTotal);
}
BEZEL_TEST(object_string_return_mint_8537,
           return_string_field_then_overwritten_spec_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var k := new Holder(); k.S := Make.Str(i); "
                      "h.O := k.GetS2(); k.S := \"zz\";"),
        kLengthTotal);
}

// ===========================================================================
// THE OTHER CONSUMERS of the result.
// ===========================================================================

// Bound to a local first, then stored.
BEZEL_TEST(object_string_return_mint_8537,
           result_bound_to_local_then_stored_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); var o := Conv.F(x); h.O := o;"),
        kLengthTotal);
}

// Handed to an `Object` formal the callee stores.
BEZEL_TEST(object_string_return_mint_8537,
           result_passed_to_storing_formal_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); h.Put(Conv.F(x));"), kLengthTotal);
}

// Stored by a CALLEE whose own `string` parameter is the source: the
// caller's release of its argument must not free the stored string.
BEZEL_TEST(object_string_return_mint_8537,
           callee_stores_result_of_its_own_string_param_spec_28_9_8_6) {
    expectNoUseAfterFree(storeThenRead("var x := Make.Str(i); h.PutRet(x);"),
                         kLengthTotal);
}
BEZEL_TEST(object_string_return_mint_8537,
           callee_stores_result_of_producer_argument_spec_28_9_8_6) {
    expectNoUseAfterFree(storeThenRead("h.PutRet(Make.Str(i));"), kLengthTotal);
}

// The result of a producer-argument call only READ: the post-call release of
// the argument used to free the string under the read.
BEZEL_TEST(object_string_return_mint_8537,
           result_of_producer_argument_read_spec_28_9_8_6) {
    expectNoUseAfterFree(
        program("      var n := Conv.F(Make.Str(i)).ToString().Length; "
                "      total := total + n; "),
        kLengthTotal);
}

// ===========================================================================
// CONTROLS — leak-checked: returns that mint nothing.
// ===========================================================================

// A literal through the same `Object` return (an rc sentinel: retain is a
// no-op).
// BOUNDARY CONTROL (base-green, kept: leak check). Goes RED under the wrong
// fix `mint at the return without the literal/+1-producer exclusion` that
// copies instead of retaining (a leaked copy per call).
BEZEL_TEST(object_string_return_mint_8537,
           control_object_return_of_literal_spec_28_9) {
    expectCleanRun(
        program("      total := total + Conv.FC(\"abc\", false).ToString().Length"
                " + Make.Str(i).Length - 2; "),
        kLengthTotal);
}

// The `string`-typed return of the same parameter is unchanged.
// BOUNDARY CONTROL (base-green, kept: leak check). Goes RED under the wrong
// fix `mint at every string-typed return`, not only the Object-typed ones.
BEZEL_TEST(object_string_return_mint_8537,
           control_string_return_of_string_param_spec_28_9) {
    expectCleanRun(
        program("      do var x := Make.Str(i); "
                "total := total + Conv.SId(x).Length; end; "),
        kLengthTotal);
}
