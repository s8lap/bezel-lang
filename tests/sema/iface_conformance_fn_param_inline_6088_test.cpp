// fix #6088 / spec §11.2 (v0.338): a member's own type parameters correspond
// by number and position, not by name, and parameter types must match exactly
// under the substitution. A function-typed PARAMETER (`(Box<T>) -> int`,
// `() -> Box<T>`) and an inline-array generic argument (`Box<[3]T>`) fell to
// typeRefEqual, which sees two declaration-less refs (`T`, `B`) as equal, so a
// positional swap was accepted. A top-level function-typed parameter of a GENERIC interface
// (`I<S>`: `(S) -> int` against `(int) -> int`) keeps the unsubstituted
// compare the §28.3 set-erasing rows s28_3_0193..0197 depend on (held out).
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

const char* kParamNeedle = "method `P` does not match interface `I`'s member `P` on parameter 1";

std::string prog(const std::string& ia, const std::string& tp,
                 const std::string& ipar, const std::string& target,
                 const std::string& kdecl, const std::string& cp,
                 const std::string& cpar, const std::string& knew,
                 const std::string& iw = "", const std::string& cw = "") {
    return std::string("module M; ") +
        "public class Box<T> public constructor() end constructor; end Box; "
        "public class Pair<K, V> public constructor() end constructor; end Pair; "
        "public interface I" + ia + " method P" + tp + "(f: " + ipar + "): int " + iw + "; end I; "
        "public class " + kdecl + ": I" + target + " public constructor() end constructor; "
        "public method P" + cp + "(f: " + cpar + "): int " + cw + " return 5; end P; end K; "
        "public class C public static method Main(): int "
        "  var s: I" + (ia.empty() ? "" : "<int>") + " := new " + knew + "(); "
        "  Console.WriteLine(7); return 0; end Main; end C;";
}

std::string own(const std::string& ipar, const std::string& cpar,
                const std::string& iw = "", const std::string& cw = "") {
    return prog("", "<T, U>", ipar, "", "K", "<A, B>", cpar, "K", iw, cw);
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
    BEZEL_EXPECT_TRUE(hasLocated(r, kParamNeedle));
}

}  // namespace

// ---- run rows ----------------------------------------------------------------

BEZEL_TEST(iface_conformance_fn_param_inline_6088, renamed_function_param_box_matches_spec_11_2) {
    expectAccept(own("(Box<T>) -> int", "(Box<A>) -> int"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, renamed_function_param_bare_matches_spec_11_2) {
    expectAccept(own("(T) -> U", "(A) -> B"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, renamed_function_param_returning_box_matches_spec_11_2) {
    expectAccept(own("() -> Box<T>", "() -> Box<A>"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, renamed_inline_array_arg_matches_spec_11_2) {
    expectAccept(own("Box<[3]T>", "Box<[3]A>"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, renamed_inline_array_param_matches_spec_11_2) {
    expectAccept(own("[3]T", "[3]A"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, renamed_second_param_inline_pair_matches_spec_11_2) {
    expectAccept(own("Pair<[2]T, [2]U>", "Pair<[2]A, [2]B>"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, constrained_renamed_function_param_matches_spec_11_2) {
    expectAccept(own("(T) -> U", "(A) -> B", "where T: hashable", "where A: hashable"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, constrained_renamed_inline_arg_matches_spec_11_2) {
    expectAccept(own("Box<[3]T>", "Box<[3]A>", "where T: new()", "where A: new()"));
}
// `K<Z>: I<Z>`: a generic-class implementer's own class parameter is the
// substitution (Z for S), through the function-typed parameter.
BEZEL_TEST(iface_conformance_fn_param_inline_6088, generic_class_implementer_function_param_matches_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I<S> method P(f: (S) -> S): int; end I; "
        "public class K<Z>: I<Z> public constructor() end constructor; "
        "  public method P(f: (Z) -> Z): int return 5; end P; end K; "
        "public class C public static method Main(): int "
        "  var s: I<int> := new K<int>(); Console.WriteLine(7); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// Generic-class implementer `K<Z>: I<Z>` / `K<Q>: I<Q>` with an inline array
// inside a generic argument: the class parameter is the substitution, applied
// through the inline array's element before the position compare.
namespace {
void expectGenericClassAccept(const std::string& iparam, const std::string& cparam,
                              const std::string& kdecl = "K<Z>: I<Z>",
                              const std::string& own = "", const std::string& cown = "") {
    auto r = runAot(
        "module M; "
        "public class Box<T> public constructor() end constructor; end Box; "
        "public class Pair<K, V> public constructor() end constructor; end Pair; "
        "public interface I<S> method P" + own + "(f: " + iparam + "): int; end I; "
        "public class " + kdecl + " public constructor() end constructor; "
        "  public method P" + cown + "(f: " + cparam + "): int return 5; end P; end K; "
        "public class C public static method Main(): int "
        "  Console.WriteLine(7); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
}  // namespace

BEZEL_TEST(iface_conformance_fn_param_inline_6088, generic_class_inline_array_in_box_matches_spec_11_2) {
    expectGenericClassAccept("Box<[3]S>", "Box<[3]Z>");
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, generic_class_inline_array_in_box_in_box_matches_spec_11_2) {
    expectGenericClassAccept("Box<Box<[3]S>>", "Box<Box<[3]Z>>");
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, generic_class_inline_array_pair_matches_spec_11_2) {
    expectGenericClassAccept("Pair<[3]S,[2]S>", "Pair<[3]Z,[2]Z>");
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, generic_class_inline_array_pair_with_own_param_matches_spec_11_2) {
    expectGenericClassAccept("Pair<[3]S,T>", "Pair<[3]Z,A>", "K<Z>: I<Z>", "<T>", "<A>");
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, generic_class_renamed_inline_array_matches_spec_11_2) {
    expectGenericClassAccept("Box<[3]S>", "Box<[3]Q>", "K<Q>: I<Q>");
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, generic_class_function_arg_matches_spec_11_2) {
    expectGenericClassAccept("Box<(S)->S>", "Box<(Z)->Z>");
}
// An instantiated interface: `I<int>`'s `Box<[3]S>` is `Box<[3]int>`.
BEZEL_TEST(iface_conformance_fn_param_inline_6088, instantiated_interface_inline_array_arg_matches_spec_11_2) {
    expectGenericClassAccept("Box<[3]S>", "Box<[3]int>", "K: I<int>");
}

// ---- reject rows -------------------------------------------------------------

BEZEL_TEST(iface_conformance_fn_param_inline_6088, function_param_box_swap_rejected_spec_11_2) {
    expectReject(own("(Box<T>) -> int", "(Box<B>) -> int"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, function_param_returning_box_swap_rejected_spec_11_2) {
    expectReject(own("() -> Box<T>", "() -> Box<B>"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, function_param_bare_swap_rejected_spec_11_2) {
    expectReject(own("(T) -> U", "(B) -> A"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, function_param_multi_swap_rejected_spec_11_2) {
    expectReject(own("(T, U) -> T", "(B, A) -> B"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, function_param_curried_swap_rejected_spec_11_2) {
    expectReject(own("(T) -> (U) -> T", "(B) -> (A) -> B"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, function_param_nullable_swap_rejected_spec_11_2) {
    expectReject(own("((T) -> U)?", "((B) -> A)?"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, function_param_concrete_slot_rejected_spec_11_2) {
    expectReject(own("(T) -> U", "(A) -> int"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, inline_array_arg_swap_rejected_spec_11_2) {
    expectReject(own("Box<[3]T>", "Box<[3]B>"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, inline_array_param_swap_rejected_spec_11_2) {
    expectReject(own("[3]T", "[3]B"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, inline_array_pair_swap_rejected_spec_11_2) {
    expectReject(own("Pair<[2]T, [2]U>", "Pair<[2]B, [2]A>"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, inline_array_length_differs_rejected_spec_11_2) {
    expectReject(own("Box<[3]T>", "Box<[4]A>"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, constrained_function_param_swap_rejected_spec_11_2) {
    expectReject(own("(T) -> U", "(B) -> A", "where T: hashable", "where A: hashable"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, constrained_inline_arg_swap_rejected_spec_11_2) {
    expectReject(own("Box<[3]T>", "Box<[3]B>", "where T: new()", "where A: new()"));
}
BEZEL_TEST(iface_conformance_fn_param_inline_6088, class_substitution_function_param_wrong_rejected_spec_11_2) {
    expectReject(prog("<S>", "", "(S) -> int", "<int>", "K", "", "(string) -> int", "K"));
}
