// fix #6200 / spec §11.2 (v0.338): a member matches an interface member only
// with "a return type that is exactly equal" ("Return types must match exactly
// (no covariance in this version)"). A generic class is checked once, at its
// declaration: `class K<Z>: B<Z>, N` reads `B<X>.Value: X` under `X -> Z`, and
// `Z` is not exactly the non-generic interface's `Value: int`. The
// non-generic property-type compare was gated on the implementing class being
// non-generic, so the declaration was accepted and `K<string>` read through
// `N` printed garbage (4447504).
//
// Reject rows assert `!r.ran` and a located §11.2 diagnostic; accept rows run
// the program and assert stdout (the member's type is exactly the interface's
// whatever Z is).

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasLocated(const JitResult& r) {
    const std::regex located(
        ":[0-9]+:[0-9]+: property `Value` implements interface `N` with a "
        "different type than the interface declares");
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            std::regex_search(d.message, located) &&
            d.message.find("11.2)") != std::string::npos)
            return true;
    return false;
}

const std::string kN =
    "module M; public interface N readonly property Value: int; end N; ";
const std::string kB =
    "public class B<X> private readonly _v: X; "
    "public constructor(v: X) _v := v; end constructor; "
    "public property Value: X => _v; end B; ";

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

}  // namespace

// --- reject: Z is not exactly int --------------------------------------------
BEZEL_TEST(iface_conformance_generic_class_nongeneric_iface_6200, forwarded_type_parameter_string_instantiation_rejects_spec_11_2_probe_33) {
    expectReject(kN + kB +
        "public class K<Z>: B<Z>, N public constructor(z: Z) base(z); end constructor; end K; "
        "public class C public static method Main(): int "
        "  var s: N := new K<string>(\"s\"); Console.WriteLine(s.Value); return 0; "
        "end Main; end C;");
}
BEZEL_TEST(iface_conformance_generic_class_nongeneric_iface_6200, forwarded_type_parameter_int_instantiation_still_rejects_at_declaration_spec_11_2) {
    expectReject(kN + kB +
        "public class K<Z>: B<Z>, N public constructor(z: Z) base(z); end constructor; end K; "
        "public class C public static method Main(): int "
        "  var s: N := new K<int>(7); Console.WriteLine(s.Value); return 0; "
        "end Main; end C;");
}
BEZEL_TEST(iface_conformance_generic_class_nongeneric_iface_6200, own_property_of_type_parameter_rejects_spec_11_2) {
    expectReject(kN +
        "public class K<Z>: N private readonly _z: Z; "
        "public constructor(z: Z) _z := z; end constructor; "
        "public property Value: Z => _z; end K; "
        "public class C public static method Main(): int "
        "  var s: N := new K<int>(7); Console.WriteLine(s.Value); return 0; "
        "end Main; end C;");
}
BEZEL_TEST(iface_conformance_generic_class_nongeneric_iface_6200, own_property_of_wrong_concrete_type_rejects_spec_11_2) {
    expectReject(kN +
        "public class K<Z>: N public constructor() end constructor; "
        "public property Value: string => \"s\"; end K; "
        "public class C public static method Main(): int "
        "  var s: N := new K<int>(); Console.WriteLine(s.Value); return 0; "
        "end Main; end C;");
}

// --- accept: the member's type does not depend on Z ---------------------------
BEZEL_TEST(iface_conformance_generic_class_nongeneric_iface_6200, own_property_of_exact_type_on_generic_class_spec_11_2) {
    expectAccept(kN +
        "public class K<Z>: N public constructor() end constructor; "
        "public property Value: int => 81; end K; "
        "public class C public static method Main(): int "
        "  var s: N := new K<string>(); Console.WriteLine(s.Value); return 0; "
        "end Main; end C;", "81\n");
}
BEZEL_TEST(iface_conformance_generic_class_nongeneric_iface_6200, generic_base_property_not_typed_by_its_parameter_spec_11_2) {
    expectAccept(kN +
        "public class B2<X> public constructor() end constructor; "
        "public property Value: int => 82; end B2; "
        "public class K<Z>: B2<Z>, N public constructor() end constructor; end K; "
        "public class C public static method Main(): int "
        "  var s: N := new K<string>(); Console.WriteLine(s.Value); return 0; "
        "end Main; end C;", "82\n");
}
BEZEL_TEST(iface_conformance_generic_class_nongeneric_iface_6200, generic_interface_over_the_same_parameter_unchanged_spec_11_2) {
    expectAccept(
        "module M; public interface G<T> readonly property Value: T; end G; " + kB +
        "public class K<Z>: B<Z>, G<Z> public constructor(z: Z) base(z); end constructor; end K; "
        "public class C public static method Main(): int "
        "  var s: G<string> := new K<string>(\"83\"); Console.WriteLine(s.Value); return 0; "
        "end Main; end C;", "83\n");
}
BEZEL_TEST(iface_conformance_generic_class_nongeneric_iface_6200, generic_base_instantiated_concretely_on_generic_class_spec_11_2) {
    expectAccept(kN + kB +
        "public class K<Z>: B<int>, N public constructor() base(84); end constructor; end K; "
        "public class C public static method Main(): int "
        "  var s: N := new K<string>(); Console.WriteLine(s.Value); return 0; "
        "end Main; end C;", "84\n");
}
