// fix #6121 / spec §11.2 (v0.338): "A member's own type parameters take part in
// the match by number and position, not by name." A class type parameter that
// the class passes inside a COMPOSITE interface argument (`K<T>: I<Box<T>>`) is
// an ordinary type of the comparison, not one of the interface method's own
// type parameters, even when it is spelled like one (`method P<T>(...)` on
// `I<S>`, class parameter `T`). #6110 recorded only the root of a substituted-in
// type, so the inner `T` of `Box<T>` was still read as the method's own `T` by
// spelling and `P<U>(f: Pair<Box<T>, U>)` was rejected on "parameter 1".
//
// Run rows assert `r.ran` and stdout; reject rows (true mismatches) assert
// `!r.ran` and a located §11.2 needle.

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

const char* kParamNeedle =
    "method `P` does not match interface `I`'s member `P` on parameter 1";
const char* kReturnNeedle =
    "method `P` does not match interface `I`'s return type";

// `I<S>` declares the members `ims` (own parameter `T`, spelled like the class
// parameter); the class `cdecl` implements them with `cms` (own parameter `W`).
std::string prog(const std::string& pre, const std::string& cdecl,
                 const std::string& inst, const std::string& newT,
                 const std::string& ims, const std::string& cms) {
    return std::string("module M; ") +
        "public class Box<T> public constructor() end constructor; end Box; "
        "public class Pair<K, V> public constructor() end constructor; end Pair; " +
        pre + "public interface I<S> " + ims + " end I; "
        "public class " + cdecl + " public constructor() end constructor; " + cms +
        " end K; "
        "public class C public static method Main(): int "
        "  var s: " + inst + " := new " + newT + "(); "
        "  Console.WriteLine(7); return 0; end Main; end C;";
}

std::string param(const std::string& itype, const std::string& ctype,
                  const std::string& cdecl = "K<T>: I<Box<T>>",
                  const std::string& inst = "I<Box<int>>",
                  const std::string& newT = "K<int>",
                  const std::string& pre = "") {
    return prog(pre, cdecl, inst, newT, "method P<T>(f: " + itype + "): int;",
                "public method P<W>(f: " + ctype + "): int return 5; end P;");
}

void expectAccept(const std::string& src) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

void expectReject(const std::string& src, const char* needle = kParamNeedle) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(r, needle));
}

const char* kJ = "public interface J<X>: I<Box<X>> end J; ";

}  // namespace

// ---- run rows ----------------------------------------------------------------

// The #6121 program.
BEZEL_TEST(iface_conformance_nested_subst_6121, box_in_pair_class_param_named_like_own_spec_11_2) {
    expectAccept(param("Pair<S, T>", "Pair<Box<T>, W>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, plain_box_class_param_named_like_own_spec_11_2) {
    expectAccept(param("Box<S>", "Box<Box<T>>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, builtin_array_class_param_named_like_own_spec_11_2) {
    expectAccept(param("Pair<Array<S>, T>", "Pair<Array<Box<T>>, W>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, box_in_box_class_param_named_like_own_spec_11_2) {
    expectAccept(param("Pair<Box<Box<S>>, T>", "Pair<Box<Box<Box<T>>>, W>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, function_typed_arg_class_param_named_like_own_spec_11_2) {
    expectAccept(param("Pair<(S) -> T, T>", "Pair<(Box<T>) -> W, W>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, inline_array_in_box_class_param_named_like_own_spec_11_2) {
    expectAccept(param("Pair<Box<[3]S>, T>", "Pair<Box<[3]Box<T>>, W>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, nullable_class_param_named_like_own_spec_11_2) {
    expectAccept(param("Pair<S?, T>", "Pair<Box<T>?, W>"));
    expectAccept(param("Pair<S, T>?", "Pair<Box<T>, W>?"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, pair_of_class_params_named_like_own_spec_11_2) {
    expectAccept(param("Pair<S, T>", "Pair<Pair<T, T>, W>", "K<T>: I<Pair<T, T>>",
                       "I<Pair<int, int>>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, two_class_params_named_like_own_spec_11_2) {
    expectAccept(param("Pair<S, T>", "Pair<Pair<T, U>, W>", "K<T, U>: I<Pair<T, U>>",
                       "I<Pair<int, string>>", "K<int, string>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, non_colliding_class_param_accepts_control_spec_11_2) {
    expectAccept(param("Pair<S, T>", "Pair<Box<Z>, W>", "K<Z>: I<Box<Z>>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, direct_class_param_accepts_control_spec_11_2) {
    expectAccept(param("Pair<S, T>", "Pair<T, W>", "K<T>: I<T>", "I<int>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, two_level_interface_class_param_named_like_own_spec_11_2) {
    expectAccept(param("Pair<S, T>", "Pair<Box<T>, W>", "K<T>: J<T>",
                       "I<Box<int>>", "K<int>", kJ));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, return_side_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("", "K<T>: I<Box<T>>", "I<Box<int>>", "K<int>",
                      "method P<T>(): Pair<S, T>?;",
                      "public method P<W>(): Pair<Box<T>, W>? return null; end P;"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, overload_with_sibling_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("", "K<T>: I<Box<T>>", "I<Box<int>>", "K<int>",
                      "method P<T>(f: Pair<S, T>): int; method P<T>(f: int): int;",
                      "public method P<W>(f: Pair<Box<T>, W>): int return 5; end P; "
                      "public method P<W>(f: int): int return 6; end P;"));
}

// ---- reject rows: true mismatches --------------------------------------------

// The implementation's own parameter sits where the class parameter belongs.
BEZEL_TEST(iface_conformance_nested_subst_6121, own_param_inside_substituted_box_rejected_spec_11_2) {
    expectReject(param("Pair<S, T>", "Pair<Box<W>, W>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, own_param_for_substituted_composite_rejected_spec_11_2) {
    expectReject(param("Pair<S, T>", "Pair<W, W>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, swapped_composite_and_own_rejected_spec_11_2) {
    expectReject(param("Pair<S, T>", "Pair<W, Box<T>>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, own_param_in_pair_class_param_rejected_spec_11_2) {
    expectReject(param("Pair<S, T>", "Pair<Pair<W, W>, W>", "K<T>: I<Pair<T, T>>",
                       "I<Pair<int, int>>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, concrete_own_slot_rejected_spec_11_2) {
    expectReject(param("Pair<S, T>", "Pair<Box<T>, int>"));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, two_level_own_param_inside_box_rejected_spec_11_2) {
    expectReject(param("Pair<S, T>", "Pair<Box<W>, W>", "K<T>: J<T>",
                       "I<Box<int>>", "K<int>", kJ));
}
BEZEL_TEST(iface_conformance_nested_subst_6121, return_side_own_param_inside_box_rejected_spec_11_2) {
    expectReject(prog("", "K<T>: I<Box<T>>", "I<Box<int>>", "K<int>",
                      "method P<T>(): Pair<S, T>?;",
                      "public method P<W>(): Pair<Box<W>, W>? return null; end P;"),
                 kReturnNeedle);
}
BEZEL_TEST(iface_conformance_nested_subst_6121, overload_sibling_own_param_inside_box_rejected_spec_11_2) {
    expectReject(prog("", "K<T>: I<Box<T>>", "I<Box<int>>", "K<int>",
                      "method P<T>(f: Pair<S, T>): int; method P<T>(f: int): int;",
                      "public method P<W>(f: Pair<Box<W>, W>): int return 5; end P; "
                      "public method P<W>(f: int): int return 6; end P;"));
}
