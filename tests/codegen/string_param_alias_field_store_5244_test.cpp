// Codegen / refcounting — issue #5244: a by-value `string` PARAMETER that
// aliases the receiver field it is stored over dangled after the second store.
// `lowerStringFieldStore` is retain-new / store / release-old; a by-value
// `string` parameter owns no +1 of its own (retainAssignedStringParams only
// mints one for a parameter the body ASSIGNS to), so for
// `c.Twice(c.Val, ...)` the second `_v := y` released the field's sole
// reference and the later read of `x` was a use-after-free (SEGV in
// bz_string_to_utf8z / heap-use-after-free under ASan).
//
// Spec: §4.6 "`string`" and §28.9 "Runtime-managed types" (the reference-
// counting implementation is unspecified; the observable contract is that a
// parameter naming a `string` value keeps reading that value for the whole
// call), spec revision v0.313.
//
// THE FIX. The caller pins every non-producer, non-literal by-value `string`
// argument across the call (retain before, release after the normal-return
// edge). Every row builds strings at RUNTIME (a literal is an rc sentinel that
// hides the bug), is loop-amplified, and asserts the exact stdout.
// runAotChecked = generated-code ASan + LeakSanitizer.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

std::string cls(const char* twiceBody) {
    return std::string(
               "module M; "
               "public class C "
               "  private _v: string; "
               "  public constructor(v: string) _v := v; end constructor; "
               "  public method Twice(x: string, y: string) ") +
           twiceBody +
           " end Twice; "
           "  public property Val: string => _v; "
           "end C; ";
}

std::string mainLoop(int iters, const std::string& loopBody) {
    return "public class Program "
           "  public static method Main(): int "
           "    using var c := new C(\"a\" + 1.ToString()); "
           "    for var i in 0.." "<" + std::to_string(iters) + " do " + loopBody +
           "    end; "
           "    Console.WriteLine(c.Val); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

std::string prevVal(int i) {
    return i == 0 ? std::string("a1") : "q" + std::to_string(i - 1);
}

std::string wantPrinted(int iters) {
    std::string w;
    for (int i = 0; i < iters; ++i) w += prevVal(i) + "\n";
    w += "q" + std::to_string(iters - 1) + "\n";
    return w;
}

const char* kTwice = "_v := x; _v := y; Console.WriteLine(x);";

std::string run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// (1) The issue's own repro: a getter lend passed to a callee that stores over
// the field twice and then reads the parameter.
BEZEL_TEST(string_param_alias_field_store_5244,
           repro_getter_lend_survives_double_store_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(cls(kTwice) + mainLoop(50,
        "c.Twice(c.Val, \"q\" + i.ToString()); ")), wantPrinted(50));
}

// (2a) Control from the issue: no second store.
BEZEL_TEST(string_param_alias_field_store_5244,
           control_single_store_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(cls("_v := x; Console.WriteLine(x);") + mainLoop(50,
        "c.Twice(c.Val, \"q\" + i.ToString()); ")),
        [] { std::string w; for (int i = 0; i < 51; ++i) w += "a1\n";
             return w; }());
}

// (2b) Control from the issue: no read of the parameter after the stores.
BEZEL_TEST(string_param_alias_field_store_5244,
           control_no_read_after_stores_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(cls("_v := x; _v := y;") + mainLoop(50,
        "c.Twice(c.Val, \"q\" + i.ToString()); ")), "q49\n");
}

// (3) Getter lend through a local: `var s := c.Val; c.Twice(s, ...)`.
BEZEL_TEST(string_param_alias_field_store_5244,
           local_borrow_arg_survives_double_store_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(cls(kTwice) + mainLoop(50,
        "var s := c.Val; c.Twice(s, \"q\" + i.ToString()); ")),
        wantPrinted(50));
}

// (4) 200 iterations under LeakSanitizer: the pin must balance exactly (a
// retain without its release leaks one string per call; an extra release
// frees the field's reference).
BEZEL_TEST(string_param_alias_field_store_5244,
           loop_200_pin_balanced_no_leak_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(cls(kTwice) + mainLoop(200,
        "c.Twice(c.Val, \"q\" + i.ToString()); ")), wantPrinted(200));
}
