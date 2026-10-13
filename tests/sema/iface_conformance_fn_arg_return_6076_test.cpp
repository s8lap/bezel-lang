// fix #6076 / spec §11.2 (v0.338): return types must match exactly under the
// substitution and a member's own type parameters correspond by position, not
// by name. A function-typed generic argument in a RETURN
// (`Array<(T) -> U>`) was compared by name, so the positional swap
// `P<A,B>(a: A): Array<(B) -> A>` was accepted for `P<T,U>(a: T): Array<(T) -> U>`.
// The return compare routes a generic return through the own-parameter-by-
// position rule with function-typed arguments compared structurally.
//
// Run rows: the renamed-correct implementation is accepted and runs; reject
// rows: positional swaps (both slots), one slot, and a concrete argument.

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasLocated(const JitResult& r, const std::string& needle) {
    const std::regex located(":[0-9]+:[0-9]+: " + needle);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            std::regex_search(d.message, located) &&
            d.message.find("(\xC2\xA7""11.2)") != std::string::npos)
            return true;
    return false;
}

std::string prog(const std::string& impl, const std::string& body) {
    return std::string("module M; ") +
        "public interface I method P<T, U>(a: T): Array<(T) -> U>; end I; "
        "public class K: I public constructor() end constructor; " + impl +
        " end K; "
        "public class C public static method Main(): int "
        "  var s: I := new K(); " + body + " return 0; end Main; end C;";
}

const char* kRetNeedle = "method `P` does not match interface `I`'s return type";

}  // namespace

BEZEL_TEST(iface_conformance_fn_arg_return_6076,
           renamed_correct_function_typed_return_matches_spec_11_2) {
    auto r = runAot(prog(
        "public method P<A, B>(a: A): Array<(A) -> B> return []; end P;",
        "var b := s.P<int, string>(1); Console.WriteLine(b.Count);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

BEZEL_TEST(iface_conformance_fn_arg_return_6076,
           same_names_function_typed_return_matches_spec_11_2) {
    auto r = runAot(prog(
        "public method P<T, U>(a: T): Array<(T) -> U> return []; end P;",
        "var b := s.P<int, string>(1); Console.WriteLine(b.Count);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

BEZEL_TEST(iface_conformance_fn_arg_return_6076,
           positional_swap_in_function_typed_return_rejected_spec_11_2) {
    auto r = runAot(prog(
        "public method P<A, B>(a: A): Array<(B) -> A> return []; end P;",
        "Console.WriteLine(1);"));
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(r, kRetNeedle));
}

BEZEL_TEST(iface_conformance_fn_arg_return_6076,
           function_typed_return_param_slot_own_param_replaced_rejected_spec_11_2) {
    auto r = runAot(prog(
        "public method P<A, B>(a: A): Array<(B) -> B> return []; end P;",
        "Console.WriteLine(1);"));
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(r, kRetNeedle));
}

BEZEL_TEST(iface_conformance_fn_arg_return_6076,
           function_typed_return_concrete_slot_rejected_spec_11_2) {
    auto r = runAot(prog(
        "public method P<A, B>(a: A): Array<(A) -> int> return []; end P;",
        "Console.WriteLine(1);"));
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(r, kRetNeedle));
}
