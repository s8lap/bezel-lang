// fix #6092 / spec §11.2 (v0.338): return types must match exactly under the
// substitution and a member's own type parameters correspond by position, not
// by name. A function type that is the WHOLE return (`(T) -> U`, curried,
// multi-parameter, nullable) was compared by name, so the positional swap
// `P<A,B>(): (B) -> A` implemented `P<T,U>(): (T) -> U`; and a class-level
// substitution (`I<int>`) was not applied through it, so `(S) -> S` was
// falsely rejected against `(int) -> int`.
//
// Run rows assert `r.ran` and stdout; reject rows assert `!r.ran` and a
// located §11.2 needle.

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

const char* kRetNeedle = "method `P` does not match interface `I`'s return type";

// Interface `I<ip>` with `method P<tp>(): ir`, implemented by K as `P<cp>(): cr`.
std::string prog(const std::string& ia, const std::string& tp,
                 const std::string& ir, const std::string& target,
                 const std::string& cp, const std::string& cr) {
    return std::string("module M; ") +
        "public interface I" + ia + " method P" + tp + "(): " + ir + "; end I; "
        "public class K: I" + target + " public constructor() end constructor; "
        "public method P" + cp + "(): " + cr + " throw new Exception(\"x\"); end P; end K; "
        "public class C public static method Main(): int "
        "  var s: I" + target + " := new K(); Console.WriteLine(7); return 0; "
        "end Main; end C;";
}

std::string own(const std::string& ir, const std::string& cr) {
    return prog("", "<T, U>", ir, "", "<A, B>", cr);
}

void expectAccept(const std::string& src) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

void expectReject(const std::string& src) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(r, kRetNeedle));
}

}  // namespace

BEZEL_TEST(iface_conformance_bare_fn_return_6092, renamed_bare_matches_spec_11_2) {
    expectAccept(own("(T) -> U", "(A) -> B"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, renamed_curried_matches_spec_11_2) {
    expectAccept(own("(T) -> (U) -> T", "(A) -> (B) -> A"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, renamed_multi_param_matches_spec_11_2) {
    expectAccept(own("(T, U) -> T", "(A, B) -> A"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, renamed_nullable_matches_spec_11_2) {
    expectAccept(own("((T) -> U)?", "((A) -> B)?"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, renamed_dictionary_fn_value_matches_spec_11_2) {
    expectAccept(own("Dictionary<T, (U) -> T>", "Dictionary<A, (B) -> A>"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, class_substitution_bare_matches_spec_11_2) {
    expectAccept(prog("<S>", "", "(S) -> S", "<int>", "", "(int) -> int"));
}

BEZEL_TEST(iface_conformance_bare_fn_return_6092, bare_swap_rejected_spec_11_2) {
    expectReject(own("(T) -> U", "(B) -> A"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, bare_mixed_concrete_param_rejected_spec_11_2) {
    expectReject(own("(int) -> U", "(int) -> A"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, multi_param_swap_rejected_spec_11_2) {
    expectReject(own("(T, U) -> T", "(B, A) -> B"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, curried_swap_rejected_spec_11_2) {
    expectReject(own("(T) -> (U) -> T", "(B) -> (A) -> B"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, nullable_swap_rejected_spec_11_2) {
    expectReject(own("((T) -> U)?", "((B) -> A)?"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, class_substitution_wrong_return_rejected_spec_11_2) {
    expectReject(prog("<S>", "", "(S) -> S", "<int>", "", "(int) -> string"));
}
BEZEL_TEST(iface_conformance_bare_fn_return_6092, bare_concrete_for_own_rejected_spec_11_2) {
    expectReject(own("(T) -> U", "(int) -> int"));
}
