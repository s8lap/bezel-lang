// fix #6110 / spec §11.2 (v0.338): "A member's own type parameters take part
// in the match by number and position, not by name." A class type parameter
// that the class passes as the interface argument (`K<T>: I<T>`) is an
// ordinary type of the comparison. It is not one of the interface method's own
// type parameters, even when it is spelled like one (`method P<T>(...)` on
// `I<S>` against the class parameter `T`): the own-parameter classification is
// by spelling, so the substituted-in `T` was read as the method's own `T` and
// the correct implementation `P<U>(Pair<T, U>)` was rejected on "parameter 1".
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

// `I<S>` declares `P<T>(f: ipar): iret`; `K<T>: I<T>` implements it as
// `P<cp>(f: cpar): cret`. The class parameter `T` collides with the interface
// method's own `T`.
std::string prog(const std::string& ipar, const std::string& cp,
                 const std::string& cpar, const std::string& iret = "int",
                 const std::string& cret = "int",
                 const std::string& body = "return 5;") {
    return std::string("module M; ") +
        "public class Box<T> public constructor() end constructor; end Box; "
        "public class Pair<K, V> public constructor() end constructor; end Pair; "
        "public interface I<S> method P<T>(f: " + ipar + "): " + iret + "; end I; "
        "public class K<T>: I<T> public constructor() end constructor; "
        "public method P" + cp + "(f: " + cpar + "): " + cret + " " + body +
        " end P; end K; "
        "public class C public static method Main(): int "
        "  var s: I<int> := new K<int>(); "
        "  Console.WriteLine(7); return 0; end Main; end C;";
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

}  // namespace

// ---- run rows ----------------------------------------------------------------

// The #6110 program: accepted before #6088, rejected by its first cut.
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, inline_array_pair_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("Pair<[3]S, [2]T>", "<U>", "Pair<[3]T, [2]U>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, inline_array_element_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("Pair<[3]S, T>", "<U>", "Pair<[3]T, U>"));
}
// The same shape without an inline array: a false reject before this fix.
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, plain_pair_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("Pair<S, T>", "<U>", "Pair<T, U>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, builtin_array_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("Array<S>", "<U>", "Array<T>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, nested_box_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("Box<Pair<S, T>>", "<U>", "Box<Pair<T, U>>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, function_typed_arg_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("Pair<(S) -> T, T>", "<U>", "Pair<(T) -> U, U>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, inline_array_function_arg_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("Pair<[3]S, (S) -> T>", "<U>", "Pair<[3]T, (T) -> U>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, bare_class_param_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("S", "<U>", "T"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, nullable_widening_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("S", "<U>", "T?"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, return_pair_class_param_named_like_own_spec_11_2) {
    expectAccept(prog("int", "<U>", "int", "Pair<S, T>?", "Pair<T, U>?",
                      "return null;"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, two_own_params_class_params_named_like_own_spec_11_2) {
    // Two class parameters spelled like two of the method's own parameters.
    expectAccept(
        "module M; "
        "public class Pair<K, V> public constructor() end constructor; end Pair; "
        "public interface I<S, R> method P<T, U>(f: Pair<Pair<S, R>, Pair<T, U>>): int; end I; "
        "public class K<T, U>: I<T, U> public constructor() end constructor; "
        "public method P<A, B>(f: Pair<Pair<T, U>, Pair<A, B>>): int return 5; end P; end K; "
        "public class C public static method Main(): int "
        "  var s: I<int, string> := new K<int, string>(); "
        "  Console.WriteLine(7); return 0; end Main; end C;");
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, own_params_renamed_and_class_param_named_like_own_spec_11_2) {
    expectAccept(
        "module M; "
        "public class Pair<K, V> public constructor() end constructor; end Pair; "
        "public interface I<S> method P<T, U>(f: Pair<[2]S, Pair<[2]T, U>>): int; end I; "
        "public class K<T>: I<T> public constructor() end constructor; "
        "public method P<A, B>(f: Pair<[2]T, Pair<[2]A, B>>): int return 5; end P; end K; "
        "public class C public static method Main(): int "
        "  var s: I<int> := new K<int>(); "
        "  Console.WriteLine(7); return 0; end Main; end C;");
}

// ---- reject rows: true mismatches --------------------------------------------

// The implementation's own `T` shadows the class `T`: the interface wants the
// class parameter at that position, the implementation has its own.
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, impl_own_param_shadows_class_param_rejected_spec_11_2) {
    expectReject(prog("Pair<S, T>", "<T>", "Pair<T, T>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, inline_array_impl_own_param_in_class_slot_rejected_spec_11_2) {
    expectReject(prog("Pair<[3]S, [2]T>", "<U>", "Pair<[3]U, [2]T>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, plain_pair_own_in_class_slot_rejected_spec_11_2) {
    expectReject(prog("Pair<S, T>", "<U>", "Pair<U, T>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, inline_array_length_differs_rejected_spec_11_2) {
    expectReject(prog("Pair<[3]S, [2]T>", "<U>", "Pair<[3]T, [4]U>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, builtin_array_own_in_class_slot_rejected_spec_11_2) {
    expectReject(prog("Array<S>", "<T>", "Array<T>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, function_typed_arg_swap_rejected_spec_11_2) {
    expectReject(prog("Pair<(S) -> T, T>", "<U>", "Pair<(U) -> T, U>"));
}
BEZEL_TEST(iface_conformance_own_vs_class_param_6110, return_own_in_class_slot_rejected_spec_11_2) {
    expectReject(prog("int", "<U>", "int", "Pair<S, T>?", "Pair<U, T>?",
                      "return null;"),
                 kReturnNeedle);
}
