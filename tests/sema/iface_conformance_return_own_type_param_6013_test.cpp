// fix #6013 / spec §11.2 (v0.338): "A member's own type parameters take part
// in the match by number and position, not by name" (§11.2 *Generic members*).
// The parameter compare already corresponds own type parameters by position
// (#5995, #5996); the return-type compare must do the same, so a member whose
// return type names its own type parameter matches an interface member that
// names the same parameter under a different spelling:
//   interface `P<T>(a: T): T` implemented as `P<X>(a: X): X` is a match.
// Mismatches stay rejected: a different return type, a positional swap of two
// own parameters, `T` against `T?` (no widening on returns, §11.2 rule 2), and
// `Array<T>` against `Array<int>`.
//
// Run rows assert `r.ran` and the program's stdout. Reject rows assert `!r.ran`
// and the §11.2 return-type needle. The renamed-return programs used to be
// rejected with "does not match interface ... return type".

#include <regex>
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

BEZEL_TEST(iface_conformance_return_own_type_param_6013,
           renamed_own_param_return_matches_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: T): T; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: X): X => a; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new K(); "
        "    Console.WriteLine(s.P<int>(42)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

BEZEL_TEST(iface_conformance_return_own_type_param_6013,
           renamed_own_param_array_return_matches_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method Wrap<T>(a: T): Array<T>; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method Wrap<X>(a: X): Array<X> "
        "    return new Array<X>(1, a); "
        "  end Wrap; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new K(); "
        "    var r := s.Wrap<int>(7); "
        "    Console.WriteLine(r[0]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(iface_conformance_return_own_type_param_6013,
           own_param_return_against_concrete_return_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: T): T; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: X): int => 0; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `P` does not match interface `I`'s return type"));
}

BEZEL_TEST(iface_conformance_return_own_type_param_6013,
           own_param_positional_swap_in_return_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T, U>(a: T): U; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<A, B>(a: A): A => a; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `P` does not match interface `I`'s return type"));
}

BEZEL_TEST(iface_conformance_return_own_type_param_6013,
           own_param_return_nullable_widening_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: T): T?; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: X): X => a; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `P` does not match interface `I`'s return type"));
}

BEZEL_TEST(iface_conformance_return_own_type_param_6013,
           array_of_own_param_against_array_of_int_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method Wrap<T>(a: T): Array<T>; "
        "end I; "
        "public class K: I "
        "  public constructor() end constructor; "
        "  public method Wrap<X>(a: X): Array<int> "
        "    return new Array<int>(1, 0); "
        "  end Wrap; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Wrap` does not match interface `I`'s return type"));
}

namespace {

// runAot carries the driver's stderr as one diagnostic message, so the source
// location is the `file:line:col: ` prefix inside that text (JitResult's
// .location is not populated). Require the prefix in front of the needle.
bool hasLocatedReturnDiagnostic(const JitResult& r) {
    const std::regex located(":[0-9]+:[0-9]+: " + std::string(
        "method `P` does not match interface `I`'s return type"));
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            std::regex_search(d.message, located) &&
            d.message.find("(\xC2\xA7""11.2)") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// Function-typed generic arguments inside a return type are compared exactly
// (§11.2 L1849), not by nullability only. A pair that carries a function type
// anywhere in its generic arguments keeps the name-based comparison, so these
// mismatches stay rejected. The programs are the reviewer's probes (i22, i23,
// i31, cc), which the #856ec29b routing accepted silently.

BEZEL_TEST(iface_conformance_return_own_type_param_6013,
           fn_type_arg_concrete_int_vs_own_param_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: T): Array<(T) -> T>; "
        "end I; "
        "public class Kls: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: X): Array<(int) -> int> throw new Exception(\"n\"); end P; "
        "end Kls; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new Kls(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedReturnDiagnostic(r));
}

BEZEL_TEST(iface_conformance_return_own_type_param_6013,
           fn_type_arg_own_vs_string_result_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: T): Array<(T) -> T>; "
        "end I; "
        "public class Kls: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: X): Array<(X) -> string> throw new Exception(\"n\"); end P; "
        "end Kls; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new Kls(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedReturnDiagnostic(r));
}

BEZEL_TEST(iface_conformance_return_own_type_param_6013,
           fn_type_unit_return_own_vs_string_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: T): Array<() -> T>; "
        "end I; "
        "public class Kls: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: X): Array<() -> string> throw new Exception(\"n\"); end P; "
        "end Kls; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new Kls(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedReturnDiagnostic(r));
}

BEZEL_TEST(iface_conformance_return_own_type_param_6013,
           fn_type_arg_own_param_vs_string_param_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface I "
        "  method P<T>(a: T): Array<(T) -> int>; "
        "end I; "
        "public class Kls: I "
        "  public constructor() end constructor; "
        "  public method P<X>(a: X): Array<(string) -> int> throw new Exception(\"n\"); end P; "
        "end Kls; "
        "public class C "
        "  public static method Main(): int "
        "    var s: I := new Kls(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedReturnDiagnostic(r));
}
