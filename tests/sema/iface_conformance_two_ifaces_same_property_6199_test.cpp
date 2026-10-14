// fix #6199 / spec §11.2 (v0.338): a member matches an interface member only
// with "the same name ... and a return type that is exactly equal" ("Return
// types must match exactly (no covariance in this version)"). A class
// implementing TWO interfaces that declare the same property name at DIFFERENT
// types therefore cannot satisfy both with one member: the diamond rule ("a
// single implementation in the implementing class satisfies all interface
// methods with the same signature") applies only to the SAME type. The
// non-generic property-type compare was keyed by property name (first writer
// wins), so it checked the member against one interface only and a read through
// the other interface SEGFAULTED (rc 139).
//
// Reject rows assert `!r.ran` and a located §11.2 diagnostic naming the
// offending interface; accept rows run the program and assert stdout (the same
// declaration reached through two paths, equal types, read-write over read-only).

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasLocated(const JitResult& r, const std::string& iface) {
    const std::regex located(
        ":[0-9]+:[0-9]+: property `Value` implements interface `" + iface +
        "` with a different type than the interface declares");
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            std::regex_search(d.message, located) &&
            d.message.find("11.2)") != std::string::npos)
            return true;
    return false;
}

const std::string kIfaces =
    "public interface N readonly property Value: int; end N; "
    "public interface N2 readonly property Value: string; end N2; ";

const std::string kMain =
    "public class C public static method Main(): int "
    "  var a: N := new K(); Console.WriteLine(a.Value); return 0; "
    "end Main; end C;";

void expectAccept(const std::string& src, const std::string& out) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

void expectReject(const std::string& src, const std::string& iface) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(hasLocated(r, iface));
}

}  // namespace

// --- reject: one member cannot have both types --------------------------------
BEZEL_TEST(iface_conformance_two_ifaces_same_property_6199, int_member_against_int_and_string_interfaces_rejects_spec_11_2) {
    expectReject("module M; " + kIfaces +
        "public class K: N, N2 public readonly property Value: int := 1; end K; " + kMain, "N2");
}
BEZEL_TEST(iface_conformance_two_ifaces_same_property_6199, interface_order_reversed_rejects_spec_11_2) {
    expectReject("module M; " + kIfaces +
        "public class K: N2, N public readonly property Value: int := 1; end K; " + kMain, "N2");
}
BEZEL_TEST(iface_conformance_two_ifaces_same_property_6199, string_member_rejects_against_the_int_interface_spec_11_2) {
    expectReject("module M; " + kIfaces +
        "public class K: N, N2 public readonly property Value: string := \"s\"; end K; " + kMain, "N");
}
BEZEL_TEST(iface_conformance_two_ifaces_same_property_6199, non_generic_base_member_rejects_spec_11_2) {
    expectReject("module M; " + kIfaces +
        "public class B public constructor() end constructor; public property Value: int => 1; end B; "
        "public class K: B, N, N2 public constructor() end constructor; end K; " + kMain, "N2");
}
BEZEL_TEST(iface_conformance_two_ifaces_same_property_6199, generic_base_instantiation_rejects_spec_11_2_probe_62) {
    expectReject("module M; " + kIfaces +
        "public class B<X> private readonly _v: X; "
        "public constructor(v: X) _v := v; end constructor; "
        "public property Value: X => _v; end B; "
        "public class K: B<int>, N, N2 public constructor() base(5); end constructor; end K; " + kMain, "N2");
}
BEZEL_TEST(iface_conformance_two_ifaces_same_property_6199, interface_extending_both_rejects_spec_11_2) {
    expectReject("module M; " + kIfaces +
        "public interface J: N, N2 end J; "
        "public class K: J public readonly property Value: int := 1; end K; " + kMain, "N2");
}

// --- accept: same type through two paths / equal types ------------------------
BEZEL_TEST(iface_conformance_two_ifaces_same_property_6199, two_interfaces_same_type_one_member_spec_11_2) {
    expectAccept("module M; "
        "public interface N readonly property Value: int; end N; "
        "public interface N3 readonly property Value: int; end N3; "
        "public class K: N, N3 public readonly property Value: int := 71; end K; "
        "public class C public static method Main(): int "
        "  var a: N := new K(); var b: N3 := new K(); "
        "  Console.WriteLine(a.Value + b.Value); return 0; end Main; end C;", "142\n");
}
BEZEL_TEST(iface_conformance_two_ifaces_same_property_6199, diamond_same_declaration_two_paths_spec_11_2) {
    expectAccept("module M; "
        "public interface N readonly property Value: int; end N; "
        "public interface J1: N end J1; public interface J2: N end J2; "
        "public class K: J1, J2 public readonly property Value: int := 72; end K; "
        "public class C public static method Main(): int "
        "  var a: J1 := new K(); var b: J2 := new K(); "
        "  Console.WriteLine(a.Value + b.Value); return 0; end Main; end C;", "144\n");
}
BEZEL_TEST(iface_conformance_two_ifaces_same_property_6199, read_write_interface_and_read_only_interface_same_type_spec_11_2) {
    expectAccept("module M; "
        "public interface R readonly property Value: int; end R; "
        "public interface W property Value: int; end W; "
        "public class K: R, W public property Value: int := 73; end K; "
        "public class C public static method Main(): int "
        "  var a: R := new K(); var b: W := new K(); "
        "  Console.WriteLine(a.Value + b.Value); return 0; end Main; end C;", "146\n");
}
BEZEL_TEST(iface_conformance_two_ifaces_same_property_6199, generic_base_with_two_equal_type_interfaces_spec_11_2) {
    expectAccept("module M; "
        "public interface N readonly property Value: int; end N; "
        "public interface N3 readonly property Value: int; end N3; "
        "public class B<X> private readonly _v: X; "
        "public constructor(v: X) _v := v; end constructor; "
        "public property Value: X => _v; end B; "
        "public class K: B<int>, N, N3 public constructor() base(74); end constructor; end K; "
        "public class C public static method Main(): int "
        "  var a: N := new K(); var b: N3 := new K(); "
        "  Console.WriteLine(a.Value + b.Value); return 0; end Main; end C;", "148\n");
}
