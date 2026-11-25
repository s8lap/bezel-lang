// Regression test for issue #9108 — `Object.ReferenceEquals` (and the
// class-qualified spelling `K.ReferenceEquals`) used as a METHOD REFERENCE at
// an explicit `(Object?, Object?) -> bool` target was REJECTED, although a
// USER static with the very same signature was admitted.
//
// THE DEFECT (a §5.1 over-rejection, measured on r30-c1 @ 116823e00 with a
// freshly built `bezelc`):
//
//   module Conf;
//   import System;
//   public class K end K;
//   public class Program
//     public static method Main(): int
//       var k := new K();
//       var h: (Object?, Object?) -> bool := Object.ReferenceEquals;
//       Console.WriteLine(h(k, k));
//       return 0;
//     end Main;
//   end Program;
//
//   re1.bzl:7:42: the initializer for `h` has a type that is not implicitly
//   convertible to the target type (§5.1)
//
// and, at a function-typed PARAMETER,
//
//   Program.Apply(K.ReferenceEquals, k, k)
//
//   re2.bzl:10:37: the argument `f` to call to `Apply` has a type that is not
//   implicitly convertible to the target type (§5.1)
//
// while the CONTROLS were admitted: `var f := Object.ReferenceEquals;`,
// `var g: (Object, Object) -> bool := Object.ReferenceEquals;`, and a USER
// static `Same(a: Object?, b: Object?): bool` at the same
// `(Object?, Object?) -> bool` target.
//
// THE ROOT CAUSE. §8.1 declares the static as
//
//     public static method ReferenceEquals(a: Object?, b: Object?): bool;
//
// and §8.4 (v0.359) says of it: "It is an ordinary static method with
// `Object?` parameters". §27.6 (v0.359) reads a method reference's FORMED
// TYPE as "the callee's own signature read as a function type" — so the
// formed type is `(Object?, Object?) -> bool`.
//
// Sema formed the reference's type by hand as `(Object, Object) -> bool` —
// NOT the declared signature — and §27.1.1's parameter bullet is
// CONTRAVARIANT ("the target's parameter type must reach the value's by a
// reference conversion, its nullable lift or the nullable view of a reference
// type, never the view's reverse"): the TARGET's `Object?` does not reach the
// VALUE's `Object` (§5.1 admits no nullable -> non-nullable conversion), so
// the reference was refused where a user static was admitted.
//
// The second half of the root cause is the ROOT CLASS itself: `Object` is a
// CLASS in the spec (§8.1: `public class Object`), so §27.1.1's class-slot
// bullet governs an `Object`/`Object?` slot — contravariant, with the
// nullable lift admitted. The compiler models the root type as the PRIMITIVE
// `PrimitiveType::Object`, and the function-value assignability check has no
// Named declaration to compare, so the root class's slots fell through to
// the "every other slot shape is INVARIANT" rule and its nullability was
// frozen into the slot.
//
// THE FIX. The formed type of the built-in static reference is the callee's
// own signature — `(Object?, Object?) -> bool` — in BOTH spellings (the root
// `Object.ReferenceEquals` and the class-qualified `K.ReferenceEquals`,
// §11.7 / §22.12's inherited static) — and the root class's slot relates as
// the class slot §27.1.1 describes.
//
// ORACLE. `runAot` / `runAotChecked` — the AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own stdout AND its exit code, and the leak row LOOPs 200
// times so that a leak cannot hide under the sanitizer's noise.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

}  // namespace

// #9108 / §8.1 / §8.4 / §27.6: the issue's own repro, verbatim — the built-in
// static `Object.ReferenceEquals` taken as a §27.6 method reference at an
// explicit `(Object?, Object?) -> bool` target.
//
// RED on the base: the program does not COMPILE ("the initializer for `h`
// has a type that is not implicitly convertible to the target type (§5.1)").
// GREEN with the fix: it compiles, and the identity compare answers `True`
// (§8.4: `Object.ReferenceEquals(a, b)` compares object identity).
BEZEL_TEST(builtin_static_method_ref_fn_target_9108,
           object_reference_equals_at_nullable_fn_target) {
    const std::string src =
        "module Conf; "
        "import System; "
        "public class K end K; "
        "public class Program "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var h: (Object?, Object?) -> bool := Object.ReferenceEquals; "
        "    Console.WriteLine(h(k, k)); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// #9108 / §8.1 / §8.4 / §11.7 / §22.12 / §27.6: the CLASS-QUALIFIED spelling
// of the same inherited static — `K.ReferenceEquals` (§11.7: "`Object`'s
// static `ReferenceEquals` (§8.1) is inherited by a static class as by every
// class"; §22.12 lets a derived type NAME it) — passed at a function-typed
// PARAMETER.
//
// RED on the base: the program does not COMPILE ("the argument `f` to call to
// `Apply` has a type that is not implicitly convertible to the target type
// (§5.1)"). GREEN with the fix.
BEZEL_TEST(builtin_static_method_ref_fn_target_9108,
           class_qualified_reference_equals_at_fn_typed_parameter) {
    const std::string src =
        "module Conf; "
        "import System; "
        "public class K end K; "
        "public class Program "
        "  public static method Apply(f: (Object?, Object?) -> bool, "
        "                       a: Object?, b: Object?): bool "
        "    return f(a, b); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    Console.WriteLine(Program.Apply(K.ReferenceEquals, k, k)); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// #9108 / §8.1 / §8.4 / §27.6 / §28.9: the reference FORMED INSIDE a loop —
// 200 fresh closure cells, one per iteration, each released at the end of its
// own scope. This is the row that pins the producer/consumer balance: the
// reference's cell must not survive its iteration (§28.9), and the identity
// compare must not buy an allocation of its own (§8.6: a reference-typed
// operand IS its own box). Same CHECKED oracle, and the loop is what keeps a
// leak from hiding under the sanitizer's noise.
//
// RED on the base: the program does not COMPILE (the annotated target).
// GREEN with the fix: 200 answers, and LeakSanitizer counts nothing.
BEZEL_TEST(builtin_static_method_ref_fn_target_9108,
           method_ref_formed_in_loop_200_no_leak) {
    const std::string src =
        "module Conf; "
        "import System; "
        "public class K end K; "
        "public class Program "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var hits := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f: (Object?, Object?) -> bool := Object.ReferenceEquals; "
        "      if f(k, k) then hits := hits + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(hits); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "200\n");
}

// #9108 / §8.1 / §27.1.1 / §27.6 — THE BOUNDARY CONTROL (the suite's one
// green control). The NON-NULLABLE target `(Object, Object) -> bool`.
//
// §27.1.1's parameter bullet: "the target's parameter type must reach the
// value's by a reference conversion, its nullable lift or the nullable view of
// a reference type" — the target's `Object` reaches the value's `Object?` by
// the nullable lift, so the reference is admitted at that target too (and the
// `Object` root class IS a class, §8.1, so the class-slot bullet governs it).
//
// This row is GREEN on the base and GREEN with the fix, and it is RED under
// the plausible WRONG fix the commit body names: forming the reference's type
// as the declared `(Object?, Object?) -> bool` WITHOUT letting the root
// class's slot relate as §27.1.1's class-slot bullet describes — that wrong
// fix rejects this program ("not implicitly convertible (§5.1)"), so the row
// pins the twin the fix must not break.
BEZEL_TEST(builtin_static_method_ref_fn_target_9108,
           object_reference_equals_at_nonnullable_fn_target_still_admitted) {
    const std::string src =
        "module Conf; "
        "import System; "
        "public class K end K; "
        "public class Program "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var g: (Object, Object) -> bool := Object.ReferenceEquals; "
        "    Console.WriteLine(g(k, k)); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}
