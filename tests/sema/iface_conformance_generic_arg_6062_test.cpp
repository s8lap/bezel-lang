// fix #6062 / spec §11.2 (v0.338): "parameter types must match exactly under
// the substitution" and a member's own type parameters correspond by number
// and position, not by name. A user-generic reference that wraps an own type
// parameter (`Box<T>`, `Array<Box<T>>`) names a declaration, so the compare
// fell through to typeRefEqual, which sees two declaration-less refs (`T`,
// `B`) as equal whatever their names: `P<T,U>(a: Box<T>)` was implemented by
// `P<A,B>(a: Box<B>)`, on the parameter and on the return side. The generic
// arguments are now compared under the own-parameter-by-position rule.
//
// Run rows assert `r.ran` and stdout: renamed own parameters, concrete
// arguments, two-parameter generics by position, nullable, nested, built-in
// containers, an interface-generic substitution and a function-typed argument
// stay accepted. Reject rows assert `!r.ran` and a located §11.2 needle.

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

const char* kPre =
    "module M; "
    "public class Box<T> public constructor() end constructor; end Box; "
    "public class Pair<K, V> public constructor() end constructor; end Pair; "
    "type BI := Box<int>; ";

std::string prog(const std::string& iface, const std::string& impl,
                 const std::string& body) {
    return std::string(kPre) +
        "public interface I " + iface + " end I; "
        "public class K: I public constructor() end constructor; " + impl +
        " end K; "
        "public class C public static method Main(): int "
        "  var s: I := new K(); " + body + " return 0; end Main; end C;";
}

}  // namespace

// ---- run rows: spec-valid, must stay accepted --------------------------------

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_renamed_own_param_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T>(a: Box<T>): int;",
        "public method P<X>(a: Box<X>): int return 1; end P;",
        "Console.WriteLine(s.P<int>(new Box<int>()));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_two_own_params_by_position_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: Box<T>): int;",
        "public method P<A,B>(a: Box<A>): int return 2; end P;",
        "Console.WriteLine(s.P<int,string>(new Box<int>()));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_concrete_arg_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P(a: Box<int>): int;",
        "public method P(a: Box<int>): int return 3; end P;",
        "Console.WriteLine(s.P(new Box<int>()));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_nested_generic_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T>(a: Box<Box<T>>): int;",
        "public method P<X>(a: Box<Box<X>>): int return 4; end P;",
        "Console.WriteLine(s.P<int>(new Box<Box<int>>()));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_pair_by_position_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: Pair<T,U>): int;",
        "public method P<A,B>(a: Pair<A,B>): int return 5; end P;",
        "Console.WriteLine(s.P<int,string>(new Pair<int,string>()));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_nullable_generic_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T>(a: Box<T>?): int;",
        "public method P<X>(a: Box<X>?): int return 6; end P;",
        "Console.WriteLine(s.P<int>(null));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_array_of_generic_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T>(a: Array<Box<T>>): int;",
        "public method P<X>(a: Array<Box<X>>): int return 7; end P;",
        "var xs: Array<Box<int>> := []; Console.WriteLine(s.P<int>(xs));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_dictionary_of_generic_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<K2,V2>(a: Dictionary<K2,Box<V2>>): int;",
        "public method P<A,B>(a: Dictionary<A,Box<B>>): int return 8; end P;",
        "var d := new Dictionary<int,Box<string>>(); Console.WriteLine(s.P<int,string>(d));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_alias_of_generic_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P(a: BI): int;",
        "public method P(a: Box<int>): int return 9; end P;",
        "Console.WriteLine(s.P(new Box<int>()));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_generic_of_function_type_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T>(a: Box<(T) -> T>): int;",
        "public method P<X>(a: Box<(X) -> X>): int return 10; end P;",
        "Console.WriteLine(s.P<int>(new Box<(int) -> int>()));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           return_renamed_own_param_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T>(a: T): owned Box<T>;",
        "public method P<X>(a: X): owned Box<X> return new Box<X>(); end P;",
        "var b := s.P<int>(1); Console.WriteLine(1);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           return_two_own_params_by_position_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: T): owned Box<T>;",
        "public method P<A,B>(a: A): owned Box<A> return new Box<A>(); end P;",
        "var b := s.P<int,string>(1); Console.WriteLine(2);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           return_array_of_generic_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: T): owned Array<Box<T>>;",
        "public method P<A,B>(a: A): owned Array<Box<A>> return []; end P;",
        "var b := s.P<int,string>(1); Console.WriteLine(b.Count);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           return_renamed_builtin_generic_matches_spec_11_2) {
    auto r = runAot(prog(
        "method P<T>(a: T): owned Array<T>;",
        "public method P<X>(a: X): owned Array<X> return []; end P;",
        "var b := s.P<int>(1); Console.WriteLine(b.Count);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// An interface-generic substitution: `I<S>` implemented as `I<int>` carries
// `Box<S>` -> `Box<int>`, accepted; `Box<string>` is a different type.
BEZEL_TEST(iface_conformance_generic_arg_6062,
           interface_generic_substitution_matches_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public class Box<T> public constructor() end constructor; end Box; "
        "public interface I<S> method P(a: Box<S>): int; end I; "
        "public class K: I<int> public constructor() end constructor; "
        "  public method P(a: Box<int>): int return 11; end P; end K; "
        "public class C public static method Main(): int "
        "  var s: I<int> := new K(); "
        "  Console.WriteLine(s.P(new Box<int>())); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           class_type_param_beside_own_param_matches_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public class Box<T> public constructor() end constructor; end Box; "
        "public interface I<S> method P<U>(a: Box<S>, b: Box<U>): int; end I; "
        "public class K<Z>: I<Z> public constructor() end constructor; "
        "  public method P<W>(a: Box<Z>, b: Box<W>): int return 12; end P; end K; "
        "public class C public static method Main(): int "
        "  var s: I<int> := new K<int>(); "
        "  Console.WriteLine(s.P<string>(new Box<int>(), new Box<string>())); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("12\n"));
}

// ---- reject rows: own parameters at different positions ----------------------

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_own_param_swap_rejected_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: Box<T>): int;",
        "public method P<A,B>(a: Box<B>): int return 1; end P;",
        ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_nested_own_param_swap_rejected_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: Box<Box<T>>): int;",
        "public method P<A,B>(a: Box<Box<B>>): int return 1; end P;",
        ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_pair_swap_rejected_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: Pair<T,U>): int;",
        "public method P<A,B>(a: Pair<B,A>): int return 1; end P;",
        ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_array_of_generic_swap_rejected_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: Array<Box<T>>): int;",
        "public method P<A,B>(a: Array<Box<B>>): int return 1; end P;",
        ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_generic_of_function_type_swap_rejected_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: Box<(T) -> T>): int;",
        "public method P<A,B>(a: Box<(B) -> B>): int return 1; end P;",
        ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           param_own_param_vs_concrete_rejected_spec_11_2) {
    auto r = runAot(prog(
        "method P<T>(a: Box<T>): int;",
        "public method P<X>(a: Box<int>): int return 1; end P;",
        ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           return_own_param_swap_rejected_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: T): owned Box<T>;",
        "public method P<A,B>(a: A): owned Box<B> return new Box<B>(); end P;",
        ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s return type"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           return_array_of_generic_swap_rejected_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: T): owned Array<Box<T>>;",
        "public method P<A,B>(a: A): owned Array<Box<B>> return []; end P;",
        ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s return type"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           return_nested_own_param_swap_rejected_spec_11_2) {
    auto r = runAot(prog(
        "method P<T,U>(a: T): owned Box<Box<T>>;",
        "public method P<A,B>(a: A): owned Box<Box<B>> return new Box<Box<B>>(); end P;",
        ""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s return type"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           interface_generic_wrong_concrete_arg_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public class Box<T> public constructor() end constructor; end Box; "
        "public interface I<S> method P(a: Box<S>): int; end I; "
        "public class K: I<int> public constructor() end constructor; "
        "  public method P(a: Box<string>): int return 1; end P; end K; "
        "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s member `P`"));
}

BEZEL_TEST(iface_conformance_generic_arg_6062,
           class_type_param_swapped_with_own_param_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public class Box<T> public constructor() end constructor; end Box; "
        "public interface I<S> method P<U>(a: Box<S>, b: Box<U>): int; end I; "
        "public class K<Z>: I<Z> public constructor() end constructor; "
        "  public method P<W>(a: Box<W>, b: Box<Z>): int return 1; end P; end K; "
        "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(
        r, "method `P` does not match interface `I`'s member `P`"));
}
