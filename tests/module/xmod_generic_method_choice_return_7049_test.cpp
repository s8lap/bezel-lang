// Regression test for issue #7049 (§14.1, §2.4, §2.8.2, v0.340).
//
// An importing module calls an imported generic METHOD at `T = int` by binding
// it to the defining module's template body, compiled against the `int` (i32)
// placeholder (§2.8.2). When that body returns a generic CHOICE built from its
// argument (`Wrap<T>(x: T): Opt<T> => Opt<T>.Some(x)`), the constructed value
// is typed as the choice's specialization struct (`choice.Opt$spec0`) while
// the function returns the base struct (`choice.Opt`). `emitReturn` treated
// every such mismatch in a template body as dead code and returned a zeroed
// value, so MEASURED on c91fbbef `Api.Wrap<int>(5)` printed `0` and
// `Mk<int>(5, 7)` printed `0,0`, both with exit code 0. The two structs are
// layout-identical, and the fix reinterprets the value instead of nulling it.
//
// §14.1: a call at explicit type arguments is an instantiation site. §2.8.2
// clause 2: "An external instantiation behaves identically to a same-module
// instantiation of the same generic at the same arguments." §2.4: an imported
// library's members are callable with their declared bodies.
//
// `T = string` and an owning class `T` are NOT the placeholder's
// representation. They keep the existing located §2.8.2 rejection (#1943 /
// #6779) rather than a wrong value; those rows pin it.
//
// Each run row uses `runAotModulesChecked` (generated-code ASan + LSan,
// exitcode 99 on a leak) and asserts the printed value.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;

namespace {

const char* kLib7049 =
    "module Lib7049;\n"
    "public choice Opt<T> Some(v: T), No; end Opt;\n"
    "public choice Pair<T> Two(a: T, b: T), Nothing; end Pair;\n"
    "public class Api\n"
    "  public static method Wrap<T>(x: T): Opt<T> => Opt<T>.Some(x);\n"
    "  public static method None<T>(): Opt<T> => Opt<T>.No;\n"
    "  public static method Mk<T>(x: T, y: T): Pair<T> => Pair<T>.Two(x, y);\n"
    "  public static method WrapBlock<T>(x: T): Opt<T>\n"
    "    var o := Opt<T>.Some(x);\n"
    "    return o;\n"
    "  end WrapBlock;\n"
    "end Api;\n";

std::string app7049(const std::string& body) {
    return "module App7049;\n"
           "import Lib7049;\n"
           "public class Item\n"
           "  public constructor() end constructor;\n"
           "end Item;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    " + body + "\n"
           "  end Main;\n"
           "end Program;\n";
}

void expectPrints(const std::string& body, const char* want) {
    JitResult r = bezel::test::runAotModulesChecked(
        {ModuleSource{"Lib7049", kLib7049},
         ModuleSource{"App7049", app7049(body)}},
        "App7049");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

void expectPlaceholderReject(const std::string& body) {
    JitResult r = bezel::test::runAotModules(
        {ModuleSource{"Lib7049", kLib7049},
         ModuleSource{"App7049", app7049(body)}},
        "App7049");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r,
        "calling imported generic method `Wrap` with a type argument whose "
        "representation is not the compiled `int` (i32) placeholder"));
}

}  // namespace

// The issue's repro: `Some` payload of an imported generic method at T = int.
BEZEL_TEST(xmod_generic_method_choice_return_7049,
           issue_repro_wrap_int_prints_payload_spec_14_1_2_4) {
    expectPrints("Console.WriteLine((case Api.Wrap<int>(5) of "
                 "Some(var v) => $\"{v}\", No => \"no\"));",
                 "5\n");
}

// The same through a block body that returns a local.
BEZEL_TEST(xmod_generic_method_choice_return_7049,
           block_body_local_wrap_int_prints_payload_spec_14_1_2_4) {
    expectPrints("Console.WriteLine((case Api.WrapBlock<int>(9) of "
                 "Some(var v) => $\"{v}\", No => \"no\"));",
                 "9\n");
}

// The payload-free case selects the `No` arm.
BEZEL_TEST(xmod_generic_method_choice_return_7049,
           no_case_selects_no_arm_spec_14_1_2_4) {
    expectPrints("Console.WriteLine((case Api.None<int>() of "
                 "Some(var v) => $\"{v}\", No => \"no\"));",
                 "no\n");
}

// A two-field payload: both fields survive (pre-fix: `0,0`).
BEZEL_TEST(xmod_generic_method_choice_return_7049,
           two_field_payload_prints_both_spec_14_1_2_4) {
    expectPrints("Console.WriteLine((case Api.Mk<int>(5, 7) of "
                 "Two(var a, var b) => $\"{a},{b}\", Nothing => \"none\"));",
                 "5,7\n");
}

// 200 iterations: the sum of payloads 0..199 is 19900 (pre-fix: 0), and LSan
// reports no leak.
BEZEL_TEST(xmod_generic_method_choice_return_7049,
           loop_200_sums_payloads_no_leak_spec_14_1_2_4) {
    expectPrints("var total := 0; "
                 "for var i in 0..<200 do "
                 "  total := total + (case Api.Wrap<int>(i) of "
                 "    Some(var v) => v, No => -1); "
                 "end; "
                 "Console.WriteLine(total);",
                 "19900\n");
}

// T = string is not the placeholder's representation: the existing located
// §2.8.2 rejection, not a wrong value.
BEZEL_TEST(xmod_generic_method_choice_return_7049,
           string_type_arg_keeps_placeholder_rejection_spec_2_8_2) {
    expectPlaceholderReject("Console.WriteLine((case Api.Wrap<string>(\"hi\") "
                            "of Some(var v) => v, No => \"no\"));");
}

// An owning class T: the same rejection.
BEZEL_TEST(xmod_generic_method_choice_return_7049,
           owning_class_type_arg_keeps_placeholder_rejection_spec_2_8_2) {
    expectPlaceholderReject("Console.WriteLine((case Api.Wrap<Item>(new Item()) "
                            "of Some(var v) => \"item\", No => \"no\"));");
}
