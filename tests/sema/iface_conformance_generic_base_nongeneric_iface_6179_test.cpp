// fix #6179 / spec §11.2 (v0.338): "Return types must match exactly (no
// covariance in this version)" and "A read-write member satisfies a read-only
// interface member of the same name and type". A property a class INHERITS from
// a generic base class (`class K: B<int>, N`) has the type `B<int>.Value`,
// which is `int`: the base's declared `X` must be read through the base's
// instantiation before the exact compare against the non-generic interface's
// `Value: int`. The untouched non-generic arm compared the unsubstituted `X`
// to `int` and falsely rejected ("property types must match exactly").
//
// Accept rows run the program (stdout read through the interface); reject rows
// assert `!r.ran` and a located §11.2 diagnostic (a real mismatch under the
// substitution still rejects).

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

const std::string kBase =
    "public class B<X> private readonly _v: X; "
    "public constructor(v: X) _v := v; end constructor; "
    "public property Value: X => _v; end B; ";

std::string prog(const std::string& iface, const std::string& decls) {
    return "module M; public interface N readonly property Value: " + iface +
           "; end N; " + kBase + decls +
           "public class C public static method Main(): int "
           "  var s: N := new K(); Console.WriteLine(s.Value); return 0; "
           "end Main; end C;";
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

}  // namespace

// --- accept ------------------------------------------------------------------
BEZEL_TEST(iface_conformance_generic_base_nongeneric_iface_6179, direct_generic_base_int_supplies_int_property_spec_11_2) {
    expectAccept(prog("int",
        "public class K: B<int>, N public constructor() base(61); end constructor; end K; "
    ), "61\n");
}
BEZEL_TEST(iface_conformance_generic_base_nongeneric_iface_6179, direct_generic_base_string_supplies_string_property_spec_11_2) {
    expectAccept(prog("string",
        "public class K: B<string>, N public constructor() base(\"ok\"); end constructor; end K; "
    ), "ok\n");
}
BEZEL_TEST(iface_conformance_generic_base_nongeneric_iface_6179, nullable_instantiation_matches_nullable_property_spec_11_2) {
    // #6778 (§7.1, v0.340): the `int?` read coalesces at `WriteLine`.
    std::string src = prog("int?",
        "public class K: B<int?>, N public constructor() base(62); end constructor; end K; ");
    const std::string print = "Console.WriteLine(s.Value);";
    src.replace(src.find(print), print.size(), "Console.WriteLine(s.Value ?? -1);");
    expectAccept(src, "62\n");
}
BEZEL_TEST(iface_conformance_generic_base_nongeneric_iface_6179, two_level_generic_base_matches_spec_11_2) {
    expectAccept(prog("int",
        "public class D: B<int> public constructor() base(63); end constructor; end D; "
        "public class K: D, N public constructor() base(); end constructor; end K; "
    ), "63\n");
}
BEZEL_TEST(iface_conformance_generic_base_nongeneric_iface_6179, non_generic_base_control_spec_11_2) {
    expectAccept(
        "module M; public interface N readonly property Value: int; end N; "
        "public class B public constructor() end constructor; public property Value: int => 64; end B; "
        "public class K: B, N public constructor() end constructor; end K; "
        "public class C public static method Main(): int "
        "  var s: N := new K(); Console.WriteLine(s.Value); return 0; "
        "end Main; end C;", "64\n");
}

// --- reject: a real mismatch under the substitution --------------------------
BEZEL_TEST(iface_conformance_generic_base_nongeneric_iface_6179, instantiation_string_against_int_rejects_spec_11_2) {
    expectReject(prog("int",
        "public class K: B<string>, N public constructor() base(\"s\"); end constructor; end K; "
    ));
}
BEZEL_TEST(iface_conformance_generic_base_nongeneric_iface_6179, nullable_instantiation_against_non_nullable_rejects_spec_11_2) {
    expectReject(prog("int",
        "public class K: B<int?>, N public constructor() base(1); end constructor; end K; "
    ));
}
BEZEL_TEST(iface_conformance_generic_base_nongeneric_iface_6179, two_level_wrong_instantiation_rejects_spec_11_2) {
    expectReject(prog("int",
        "public class D: B<string> public constructor() base(\"s\"); end constructor; end D; "
        "public class K: D, N public constructor() base(); end constructor; end K; "
    ));
}
BEZEL_TEST(iface_conformance_generic_base_nongeneric_iface_6179, instantiation_int_against_string_rejects_spec_11_2) {
    expectReject(prog("string",
        "public class K: B<int>, N public constructor() base(1); end constructor; end K; "
    ));
}
