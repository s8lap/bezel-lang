// Codegen — issue #4799: `Same<T>(a: T, b: T) => a.Equals(b)` over a generic
// record `Pair<T>` printed `False` for EQUAL pairs. Renaming only the record's
// parameter (`record Pair<U>(A: U, B: U)`) printed the right values, so the
// defect is type-parameter NAME shadowing, not a missing monomorphization.
//
// AUTHORITY. Spec §9.1 (v0.307): a positional record synthesizes
// `Equals(T)` "over the same participating positional fields, agreeing with
// the generated `=` (§8.3)". §8.3 fixes that equality compares the
// components. A value that depends on what the callee's type parameter
// happens to be NAMED is wrong code.
//
// MECHANISM. `lowerValueSemMethodOn`'s record arm installs a
// `RecordSubstGuard` — which REPLACES the substitution with the record's own
// param→arg map (`T` → `int`) — and only then resolved and lowered the
// ARGUMENT. The argument's type is the CALLEE's `T` (bound to `Pair<int>`),
// so it was read as the record's `T` = `int`; `equalsArgSettlesFalse` then
// answered a static `False` (an int is never a Pair). The fix resolves and
// lowers the argument under the caller's substitution.
//
// FAIL-WITHOUT (measured): with the withCallerSubst hunk of
// src/codegen/LlvmBackend_Calls2.cpp reverted and the test binary rebuilt,
// the int, string and loop rows FAIL (`False` for the equal pairs; loop 0).
// renamed_param_control passes on both sides by design.
//
// §9.1 / §8.3, v0.307.

#include <string>

#include "support/aot_support.hpp"   // runAot
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string out(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + ">" + r.stdoutText;
    return r.stdoutText;
}

std::string program(const char* recordParam, const char* body) {
    return std::string("module M; public record Pair<") + recordParam + ">(A: " +
           recordParam + ", B: " + recordParam + "); "
           "public static class Helpers "
           "  public static method Same<T>(a: T, b: T): bool => a.Equals(b); "
           "end Helpers; "
           "public class Program "
           "  public static method Main() " + body +
           "  end Main; "
           "end Program;";
}
}  // namespace

// The issue's repro: record param and callee param both named `T`.
BEZEL_TEST(generic_record_equals_shadowed_param_4799, same_name_t_pair_int_spec_9_1) {
    BEZEL_EXPECT_EQ(out(program("T",
        "    Console.WriteLine(Helpers.Same(new Pair<int>(1, 2), new Pair<int>(1, 2))); "
        "    Console.WriteLine(Helpers.Same(new Pair<int>(1, 2), new Pair<int>(1, 3))); ")),
        std::string("True\nFalse\n"));
}

// String components compare by content (§8.3) — distinct allocations.
BEZEL_TEST(generic_record_equals_shadowed_param_4799, same_name_t_pair_string_spec_8_3) {
    BEZEL_EXPECT_EQ(out(program("T",
        "    Console.WriteLine(Helpers.Same(new Pair<string>(\"a\", \"b\"), new Pair<string>(\"a\" + \"\", \"b\"))); "
        "    Console.WriteLine(Helpers.Same(new Pair<string>(\"a\", \"b\"), new Pair<string>(\"b\", \"b\"))); ")),
        std::string("True\nFalse\n"));
}

// 50 iterations, both truth values each iteration: exactly half are equal.
BEZEL_TEST(generic_record_equals_shadowed_param_4799, same_name_t_loop_50_spec_9_1) {
    BEZEL_EXPECT_EQ(out(program("T",
        "    var hits := 0; "
        "    for var i in 0..<50 do "
        "      if Helpers.Same(new Pair<int>(i, i mod 2), new Pair<int>(i, 0)) then hits := hits + 1; end; "
        "    end; "
        "    Console.WriteLine(hits); ")),
        std::string("25\n"));
}

// Control: the record parameter renamed to `U` — correct before and after.
BEZEL_TEST(generic_record_equals_shadowed_param_4799, renamed_param_control_spec_9_1) {
    BEZEL_EXPECT_EQ(out(program("U",
        "    Console.WriteLine(Helpers.Same(new Pair<int>(1, 2), new Pair<int>(1, 2))); "
        "    Console.WriteLine(Helpers.Same(new Pair<int>(1, 2), new Pair<int>(1, 3))); ")),
        std::string("True\nFalse\n"));
}
