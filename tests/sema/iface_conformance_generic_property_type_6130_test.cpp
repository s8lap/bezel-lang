// fix #6130 / spec §11.2 (v0.338): "Return types must match exactly (no
// covariance in this version)" and "A read-write member satisfies a read-only
// interface member of the same name and type", read under the interface's
// substitution. The property-type compare skipped every generic interface
// (`if (!iface || !iface->genericParams.empty()) continue;`), so
// `I<T>: readonly property Value: T` implemented as `property Value: string`
// under `I<int>` was accepted (rc 0, no diagnostic); the non-generic case
// already rejects ("property types must match exactly (no covariance)").
//
// Reject rows assert `!r.ran` and a located §11.2 diagnostic; accept controls
// run the program and assert stdout read through the interface.

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasLocated(const JitResult& r) {
    const std::regex located(
        ":[0-9]+:[0-9]+: property `Value` implements interface `[A-Za-z]+` "
        "with a (positional field of a )?different type than the interface "
        "declares");
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            std::regex_search(d.message, located) &&
            d.message.find("11.2)") != std::string::npos)
            return true;
    return false;
}

// `decls` declares the interfaces and K; Main reads `Value` through `I`
// (spelled `iref`, e.g. `I` or `I<int>`).
std::string withMain(const std::string& decls, const std::string& iref) {
    return "module M; " + decls +
        "public class C public static method Main(): int "
        "  var s: " + iref + " := new K(); Console.WriteLine(s.Value); return 0; "
        "end Main; end C;";
}

std::string klass(const std::string& heads, const std::string& type,
                  const std::string& body) {
    return "public class K: " + heads + " public constructor() end constructor; "
           "public property Value: " + type + " => " + body + "; end K; ";
}

void expectAccept(const std::string& src, const std::string& out) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

void expectReject(const std::string& src) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(r));
}

const std::string kGen =
    "public interface I<T> readonly property Value: T; end I; ";

}  // namespace

// --- accept controls ---------------------------------------------------------------
BEZEL_TEST(iface_conformance_generic_property_type_6130, int_under_substitution_matches_spec_11_2) {
    expectAccept(withMain(kGen + klass("I<int>", "int", "51"), "I<int>"), "51\n");
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, string_under_substitution_matches_spec_11_2) {
    expectAccept(withMain(kGen + klass("I<string>", "string", "\"ok\""), "I<string>"), "ok\n");
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, nested_generic_matches_spec_11_2) {
    // `Array<T>` at T = int is `Array<int>`; the getter is never called.
    expectAccept(
        "module M; public interface I<T> readonly property Value: Array<T>; end I; "
        "public class K: I<int> public constructor() end constructor; "
        "public property Value: Array<int> => throw new Exception(\"x\"); end K; "
        "public class C public static method Main(): int "
        "  var s: I<int> := new K(); Console.WriteLine(56); return 0; "
        "end Main; end C;", "56\n");
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, nullable_under_substitution_matches_spec_11_2) {
    expectAccept(withMain(
        "public interface I<T> readonly property Value: T?; end I; " +
        klass("I<int>", "int?", "52"), "I<int>"), "52\n");
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, read_write_satisfies_read_only_spec_11_2) {
    // §11.2: "A read-write member satisfies a read-only interface member of the
    // same name and type".
    expectAccept(withMain(
        kGen + "public class K: I<int> public constructor() end constructor; "
        "public property Value: int := 53; end K; ", "I<int>"), "53\n");
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, extending_generic_matches_spec_11_2) {
    expectAccept(withMain(
        "public interface J<T> readonly property Value: T; end J; "
        "public interface I: J<int> end I; " + klass("I", "int", "54"), "I"), "54\n");
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, generic_class_forwarding_matches_spec_11_2) {
    expectAccept(
        "module M; " + kGen +
        "public class K<T>: I<T> public constructor() end constructor; "
        "public property Value: T => throw new Exception(\"x\"); end K; "
        "public class C public static method Main(): int "
        "  var s: I<int> := new K<int>(); Console.WriteLine(55); return 0; "
        "end Main; end C;", "55\n");
}

// --- reject rows ----------------------------------------------------------------------
BEZEL_TEST(iface_conformance_generic_property_type_6130, issue_repro_int_vs_string_rejected_spec_11_2) {
    expectReject(withMain(kGen + klass("I<int>", "string", "\"x\""), "I<int>"));
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, nullable_for_plain_rejected_spec_11_2) {
    expectReject(withMain(kGen + klass("I<int>", "int?", "1"), "I<int>"));
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, plain_for_nullable_rejected_spec_11_2) {
    expectReject(withMain(kGen + klass("I<int?>", "int", "1"), "I<int?>"));
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, nested_argument_rejected_spec_11_2) {
    expectReject(withMain(
        "public interface I<T> readonly property Value: Array<T>; end I; "
        "public class K: I<int> public constructor() end constructor; "
        "public property Value: Array<string> => throw new Exception(\"x\"); end K; ",
        "I<int>"));
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, extending_generic_rejected_spec_11_2) {
    expectReject(withMain(
        "public interface J<T> readonly property Value: T; end J; "
        "public interface I: J<int> end I; " + klass("I", "string", "\"x\""), "I"));
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, two_level_extending_rejected_spec_11_2) {
    expectReject(withMain(
        "public interface J<T> readonly property Value: T; end J; "
        "public interface M<U>: J<U> end M; "
        "public interface I: M<int> end I; " + klass("I", "string", "\"x\""), "I"));
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, positional_record_field_rejected_spec_11_2) {
    expectReject(
        "module M; " + kGen +
        "public record K(Value: string): I<int>; "
        "public class C public static method Main(): int "
        "  return 0; end Main; end C;");
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, generic_class_forwarding_wrong_type_rejected_spec_11_2) {
    expectReject(
        "module M; " + kGen +
        "public class K<T>: I<T> public constructor() end constructor; "
        "public property Value: string => \"x\"; end K; "
        "public class C public static method Main(): int "
        "  return 0; end Main; end C;");
}
BEZEL_TEST(iface_conformance_generic_property_type_6130, inherited_base_property_rejected_spec_11_2) {
    expectReject(withMain(
        kGen + "public class B public constructor() end constructor; "
        "public property Value: string => \"x\"; end B; "
        "public class K: B, I<int> public constructor() end constructor; end K; ",
        "I<int>"));
}
