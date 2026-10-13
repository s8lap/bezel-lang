// fix #5995 / spec §11.2 (v0.338, *Generic members*, L1849): "A member's own
// type parameters take part in the match by number and position, not by name.
// ... each of the interface member's own type parameters [maps] to the
// candidate's type parameter at the same position ... So `public method
// Show<U>(x: U): string` matches `method Show<T>(x: T): string;`, while
// `public method Pair<T, U>(a: U, b: T): int` does not match `method
// Pair<T, U>(a: T, b: U): int;`."
//
// Two bare own type parameters match iff they sit at the same position in
// their member's generic parameter list; renaming in order is accepted,
// swapping positions is rejected.
//
// Reject rows assert `!r.ran` and the §11.2 needle. Run rows assert `r.ran`
// and the program's stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasConformanceDiagnostic(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos &&
            d.message.find("(\xC2\xA7""11.2)") != std::string::npos)
            return true;
    return false;
}

}  // namespace

BEZEL_TEST(iface_conformance_method_type_param_position_5995,
           swapped_dictionary_arguments_rejected_spec_11_2) {
    // The issue repro: `Dictionary<Y, X>` against `Dictionary<A, B>` maps
    // A -> X, B -> Y, so the candidate's parameter is `Dictionary<Y, X>` and
    // not `Dictionary<X, Y>`.
    auto r = runAot(
        "module M; "
        "public interface I<V> "
        "  method Show<A, B>(d: Dictionary<A, B>); "
        "end I; "
        "public class K: I<int> "
        "  public constructor() end constructor; "
        "  public method Show<X, Y>(d: Dictionary<Y, X>) "
        "  end Show; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Show` does not match interface `I`'s member `Show`"));
}

BEZEL_TEST(iface_conformance_method_type_param_position_5995,
           spec_pair_swapped_params_rejected_spec_11_2) {
    // The spec's own example (L1849): `Pair<T, U>(a: U, b: T)` does not match
    // `Pair<T, U>(a: T, b: U)`.
    auto r = runAot(
        "module M; "
        "public interface IPair "
        "  method Pair<T, U>(a: T, b: U): int; "
        "end IPair; "
        "public class K: IPair "
        "  public constructor() end constructor; "
        "  public method Pair<T, U>(a: U, b: T): int "
        "    return 1; "
        "  end Pair; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Pair` does not match interface `IPair`'s member `Pair`"));
}

BEZEL_TEST(iface_conformance_method_type_param_position_5995,
           spec_pair_in_order_renamed_runs_spec_11_2) {
    // The accept half of the spec example: the same positions under other
    // names match, and the call reaches the implementation.
    auto r = runAot(
        "module M; "
        "public interface IPair "
        "  method Pair<T, U>(a: T, b: U): int; "
        "end IPair; "
        "public class K: IPair "
        "  public constructor() end constructor; "
        "  public method Pair<P, Q>(a: P, b: Q): int "
        "    return 42; "
        "  end Pair; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var s: IPair := new K(); "
        "    Console.WriteLine(s.Pair<int, string>(1, \"x\").ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

BEZEL_TEST(iface_conformance_method_type_param_position_5995,
           dictionary_arguments_renamed_in_order_runs_spec_11_2) {
    // `Show<X, Y>(d: Dictionary<X, Y>)` witnesses `Show<A, B>(d:
    // Dictionary<A, B>)` (A -> X, B -> Y).
    auto r = runAot(
        "module M; "
        "public interface I<V> "
        "  method Show<A, B>(d: Dictionary<A, B>): int; "
        "end I; "
        "public class K: I<int> "
        "  public constructor() end constructor; "
        "  public method Show<X, Y>(d: Dictionary<X, Y>): int "
        "    return d.Count; "
        "  end Show; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); "
        "    d.Add(\"b\", 2); "
        "    var s: I<int> := new K(); "
        "    Console.WriteLine(s.Show<string, int>(d).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

BEZEL_TEST(iface_conformance_method_type_param_position_5995,
           single_param_renamed_runs_spec_11_2) {
    // `Show<U>(x: U)` still matches `Show<T>(x: T)` (L1849).
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method Show<T>(x: T): string; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method Show<U>(x: U): string "
        "    return \"ok\"; "
        "  end Show; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new K(); "
        "    Console.WriteLine(s.Show<int>(5)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}
