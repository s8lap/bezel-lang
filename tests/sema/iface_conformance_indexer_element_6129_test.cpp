// fix #6129 / spec §11.2 (v0.338): "Return types must match exactly (no
// covariance in this version)" and "A read-write member satisfies a read-only
// interface member of the same name and type". An indexer's ELEMENT type is its
// return. The interface-conformance check picked the implementing indexer by
// its parameter list and never compared the element type, so
// `readonly property [i: int]: int` was satisfied by `property [i: int]: string`
// (rc 0, no diagnostic). The non-generic named-property twin already rejects
// ("property types must match exactly (no covariance)", §11.2).
//
// The indexer PARAMETER compare is deliberately not touched (#6131).
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
        ":[0-9]+:[0-9]+: indexer implements interface `I` with a different "
        "element type than the interface declares");
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            std::regex_search(d.message, located) &&
            d.message.find("(\xC2\xA7""11.2") != std::string::npos)
            return true;
    return false;
}

// Interface `I<ia>` with an indexer of element type `ie` (`ro` -> readonly),
// implemented by `K: I<target>` whose indexer has element type `ce` returning
// `body`. Main reads index 0 through the interface and prints it.
std::string prog(const std::string& ia, const std::string& ie, bool ro,
                 const std::string& target, const std::string& ce,
                 const std::string& body) {
    return std::string("module M; ") +
        "public interface I" + ia + " " + (ro ? "readonly " : "") +
        "property [i: int]: " + ie + "; end I; "
        "public class K: I" + target + " public constructor() end constructor; " +
        (ro ? "public property [i: int]: " + ce + " => " + body + "; "
            : "public property [i: int]: " + ce + " get => " + body +
              "; set => do end; end; ") +
        "end K; "
        "public class C public static method Main(): int "
        "  var s: I" + target + " := new K(); Console.WriteLine(s[0]); return 0; "
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

// --- accept controls: the element type matches ---------------------------------
BEZEL_TEST(iface_conformance_indexer_element_6129, same_type_matches_spec_11_2) {
    expectAccept(prog("", "int", true, "", "int", "41"), "41\n");
}
BEZEL_TEST(iface_conformance_indexer_element_6129, string_element_matches_spec_11_2) {
    expectAccept(prog("", "string", true, "", "string", "\"ab\""), "ab\n");
}
BEZEL_TEST(iface_conformance_indexer_element_6129, generic_substitution_matches_spec_11_2) {
    expectAccept(prog("<T>", "T", true, "<int>", "int", "42"), "42\n");
}
BEZEL_TEST(iface_conformance_indexer_element_6129, generic_nested_substitution_matches_spec_11_2) {
    // `Array<T>` at T = int is `Array<int>`; the getter is never called.
    expectAccept(
        "module M; public interface I<T> readonly property [i: int]: Array<T>; end I; "
        "public class K: I<int> public constructor() end constructor; "
        "public property [i: int]: Array<int> => throw new Exception(\"x\"); end K; "
        "public class C public static method Main(): int "
        "  var s: I<int> := new K(); Console.WriteLine(43); return 0; "
        "end Main; end C;", "43\n");
}
BEZEL_TEST(iface_conformance_indexer_element_6129, read_write_satisfies_read_only_spec_11_2) {
    // §11.2: "A read-write member satisfies a read-only interface member of the
    // same name and type".
    std::string src =
        "module M; public interface I readonly property [i: int]: int; end I; "
        "public class K: I public constructor() end constructor; "
        "public property [i: int]: int get => 44; set => do end; end; end K; "
        "public class C public static method Main(): int "
        "  var s: I := new K(); Console.WriteLine(s[0]); return 0; "
        "end Main; end C;";
    expectAccept(src, "44\n");
}
BEZEL_TEST(iface_conformance_indexer_element_6129, read_write_interface_matches_spec_11_2) {
    expectAccept(prog("", "int", false, "", "int", "45"), "45\n");
}

// --- reject rows: the element type differs --------------------------------------
BEZEL_TEST(iface_conformance_indexer_element_6129, issue_repro_int_vs_string_rejected_spec_11_2) {
    expectReject(prog("", "int", true, "", "string", "\"x\""));
}
BEZEL_TEST(iface_conformance_indexer_element_6129, read_write_interface_int_vs_string_rejected_spec_11_2) {
    expectReject(prog("", "int", false, "", "string", "\"x\""));
}
BEZEL_TEST(iface_conformance_indexer_element_6129, nullable_element_rejected_spec_11_2) {
    // Property types match exactly: `int?` does not satisfy `int`.
    expectReject(prog("", "int", true, "", "int?", "1"));
}
BEZEL_TEST(iface_conformance_indexer_element_6129, non_nullable_for_nullable_rejected_spec_11_2) {
    expectReject(prog("", "int?", true, "", "int", "1"));
}
BEZEL_TEST(iface_conformance_indexer_element_6129, generic_element_under_substitution_rejected_spec_11_2) {
    expectReject(prog("<T>", "T", true, "<int>", "string", "\"x\""));
}
BEZEL_TEST(iface_conformance_indexer_element_6129, generic_element_nullable_under_substitution_rejected_spec_11_2) {
    expectReject(prog("<T>", "T", true, "<int>", "int?", "1"));
}
BEZEL_TEST(iface_conformance_indexer_element_6129, generic_array_element_rejected_spec_11_2) {
    expectReject(prog("<T>", "Array<T>", true, "<int>", "Array<string>",
                      "throw new Exception(\"x\")"));
}
