// fix #6053 / spec §11.2 (v0.338): "parameter types must match exactly under
// the substitution", and a member's own type parameters correspond by number
// and position. A function-typed generic argument (`Array<(T) -> T>`) was
// compared by nullability only, so an implementer whose function type carries
// a different concrete type (`Array<(int) -> int>` against an interface's
// `Array<(T) -> T>`) was accepted. The function type is now compared
// structurally (parameters, return, modifiers and nullability), with each
// component routed through the same own-parameter rule.
//
// Run rows assert `r.ran` and the program's stdout; they keep the spec-valid
// shapes accepted (same-shape renaming, zero-argument, unit return, nested
// function return, nullable return, a user generic class around a function
// type, and a class type parameter inside a function type). Reject rows assert
// `!r.ran` and a located §11.2 parameter-type needle.

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// runAot carries the driver's stderr as one diagnostic message; require the
// `file:line:col: ` prefix in front of the needle.
bool hasLocatedParamDiagnostic(const JitResult& r, const std::string& needle) {
    const std::regex located(":[0-9]+:[0-9]+: " + needle);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            std::regex_search(d.message, located) &&
            d.message.find("(\xC2\xA7""11.2)") != std::string::npos)
            return true;
    return false;
}

const char* kMainNoop =
    "public class C "
    "  public static method Main(): int "
    "    var s: I := new K(); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ---- run rows: spec-valid, must stay accepted --------------------------------

BEZEL_TEST(iface_conformance_function_arg_6053,
           same_shape_renamed_function_arg_matches_spec_11_2) {
    // `Array<(T) -> T>` against `Array<(X) -> X>`: the own parameter corresponds
    // by position, so the renamed function type matches.
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: Array<(T) -> T>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Array<(X) -> X>): int return a.Count; end P; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new K(); "
        "    var fs: Array<(int) -> int> := []; "
        "    Console.WriteLine(s.P<int>(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           zero_argument_function_arg_matches_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: Array<() -> T>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Array<() -> X>): int return a.Count; end P; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new K(); "
        "    var fs: Array<() -> int> := []; "
        "    Console.WriteLine(s.P<int>(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           unit_return_function_arg_matches_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: Array<(T) -> ()>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Array<(X) -> ()>): int return a.Count; end P; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new K(); "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           nested_function_return_matches_spec_11_2) {
    // `(T) -> (T) -> T` against `(X) -> (X) -> X`: the function return is itself
    // a function type, compared by the same structural rule.
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: Array<(T) -> (T) -> T>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Array<(X) -> (X) -> X>): int return a.Count; end P; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new K(); "
        "    var fs: Array<(int) -> (int) -> int> := []; "
        "    Console.WriteLine(s.P<int>(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           nullable_function_return_matches_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: Array<(T) -> T?>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Array<(X) -> X?>): int return a.Count; end P; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new K(); "
        "    var fs: Array<(int) -> int?> := []; "
        "    Console.WriteLine(s.P<int>(fs)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           user_generic_box_of_function_arg_matches_spec_11_2) {
    // A declaration-bearing user generic around a function type takes the
    // typesMatch path; the renamed own parameter must still correspond by
    // position inside the box.
    auto r = runAot(
        "module M; "
        "public class Box<Q> "
        "  public constructor() end constructor; "
        "end Box; "
        "public interface I "
        "  method P<T>(a: Box<(T) -> T>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Box<(X) -> X>): int return 1; end P; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new K(); "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           class_param_inside_function_arg_substituted_spec_11_2) {
    // The interface's class-level `C` sits inside the function type; the
    // substitution `C := int` from `I<int>` must reach it before the compare.
    auto r = runAot(
        "module M; "
        "public interface I<C> "
        "  method P<T>(a: Array<(T) -> C>): int; "
        "end I; "
        "public class K: I<int> "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Array<(X) -> int>): int return a.Count; end P; "
        "end K; "
        "public class C0 "
        "  public static method Main(): int "
        "    var s: I<int> := new K(); "
        "    var fs: Array<(string) -> int> := []; "
        "    Console.WriteLine(s.P<string>(fs)); "
        "    return 0; "
        "  end Main; "
        "end C0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ---- reject rows: function types that differ under the substitution ----------

BEZEL_TEST(iface_conformance_function_arg_6053,
           own_param_against_concrete_function_arg_rejected_spec_11_2) {
    // Issue repro: `Array<(T) -> T>` is not `Array<(int) -> int>`; the own `T`
    // corresponds only to an own parameter.
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: Array<(T) -> T>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Array<(int) -> int>): int return 1; end P; "
        "end K; " +
        std::string(kMainNoop));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedParamDiagnostic(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           function_return_concrete_against_own_param_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: Array<(int) -> T>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Array<(int) -> int>): int return 1; end P; "
        "end K; " +
        std::string(kMainNoop));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedParamDiagnostic(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           function_positional_swap_of_own_params_rejected_spec_11_2) {
    // `(T) -> U` against `(B) -> A`: the own parameters swap positions.
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T, U>(a: Array<(T) -> U>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<A, B>(a: Array<(B) -> A>): int return 1; end P; "
        "end K; " +
        std::string(kMainNoop));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedParamDiagnostic(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           function_return_nullability_differs_rejected_spec_11_2) {
    // A nullable function return is a different parameter type (§11.2 rule 2
    // gives no widening inside a function type).
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: Array<(T) -> T>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Array<(X) -> X?>): int return 1; end P; "
        "end K; " +
        std::string(kMainNoop));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedParamDiagnostic(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           function_arity_differs_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: Array<(T) -> T>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: Array<(X, X) -> X>): int return 1; end P; "
        "end K; " +
        std::string(kMainNoop));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedParamDiagnostic(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_function_arg_6053,
           non_generic_function_arg_concrete_mismatch_rejected_spec_11_2) {
    // No own parameters at all: `(int) -> int` against `(string) -> int` was
    // accepted by the nullability-only compare.
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P(a: Array<(int) -> int>): int; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P(a: Array<(string) -> int>): int return 1; end P; "
        "end K; " +
        std::string(kMainNoop));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedParamDiagnostic(
        r, "method `P` does not match interface `I`'s member `P`"));
}
