// Remedy text of the borrow-only arm-binding diagnostics after the marked named
// slot was retired (issue #5938; spec §19.3 "Named-slot bindings and
// ownership", §35.5, v0.338, proposal 0183).
//
// THE RULE. §19.3 (v0.338): "A named slot **never transfers** ... A program
// that wants an owned or move-only value payload transferred writes the
// **positional** destructure, which transfers every obligation-bearing slot at
// once ...; `move` is not a slot spelling (`Fetched(move response = var r)` is
// a compile error at the pattern naming the positional form — retired
// v0.338)." §35.5: "A program wanting the payload transferred writes the
// positional destructure."
//
// THE BUG. Two ownership diagnostics (a `move` of a borrow-only arm binding,
// and a plain store of a borrow-only move-only arm binding) still advised "a
// marked named slot" as the transferring form. Following that advice is a
// parse error. Each diagnostic gets three rows: the new remedy text, the
// ADVISED remedy (positional destructure) compiling and running correctly
// under ASan (`runAotChecked`), and the retired spelling being a specific
// parse error.
//
// The sema twin ("write the positional destructure over the case, or a marked
// named slot on it", record/class head) lived inside the marker-admission
// block, which is unreachable once the parser rejects the marker; that block
// was removed, so it has no row here.

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked — strict AOT
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// Diagnostic 1: `move` of an UNMARKED named-slot binding of an `owned` class
// payload in an owning match (conformance row s35_5_0010's shape).
std::string ownedPayloadProgram(const char* slot) {
    return std::string(
               "module M; "
               "public class Widget "
               "  public constructor() end constructor; "
               "  destructor Console.WriteLine(\"widget disposed\"); "
               "    end destructor; "
               "end Widget; "
               "public choice Outcome "
               "  Fetched(owned response: Widget), "
               "  Failed(status: int); "
               "end Outcome; "
               "public class Client "
               "  public constructor() end constructor; "
               "  public method Fetch(): Outcome => "
               "    Outcome.Fetched(new Widget()); "
               "end Client; "
               "public class Program "
               "  public static method Main() "
               "    using var client := new Client(); "
               "    case client.Fetch() of ") +
           slot +
           " => do using var w := move r; end, "
           "      Failed(_) => Console.WriteLine(\"failed\") "
           "    end case; "
           "  end Main; "
           "end Program;";
}

// Diagnostic 2: a plain store of an UNMARKED named-slot binding of a
// move-only (nested owning choice) payload in an owning match.
std::string moveOnlyPayloadProgram(const char* slot) {
    return std::string(
               "module M; "
               "public class Widget "
               "  public constructor() end constructor; "
               "  destructor Console.WriteLine(\"dispose\"); end destructor; "
               "end Widget; "
               "public choice Outer W(owned a: Widget, owned b: Widget), X; "
               "end Outer; "
               "public choice Box Wrap(inner: Outer), Nothing; end Box; "
               "public class Program "
               "  public static method Main(): int "
               "    var bx := Box.Wrap(Outer.W(new Widget(), new Widget())); "
               "    case move bx of ") +
           slot +
           " => do var z := i; end, "
           "      Nothing => () "
           "    end case; "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// --- Diagnostic 1: `move` of a borrow-only arm binding ---------------------

BEZEL_TEST(positional_pattern_named_slot_remedy_5938,
           spec_19_3_move_of_named_slot_binding_remedy_names_positional_form) {
    auto r = runAot(ownedPayloadProgram("Fetched(response = var r)"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "write the positional destructure, which transfers it"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "marked named slot"));
}

BEZEL_TEST(positional_pattern_named_slot_remedy_5938,
           spec_19_3_move_of_named_slot_binding_advised_positional_form_runs) {
    auto r = runAotChecked(ownedPayloadProgram("Fetched(var r)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "widget disposed\n");
}

BEZEL_TEST(positional_pattern_named_slot_remedy_5938,
           spec_19_3_move_marker_on_named_slot_is_a_parse_error) {
    auto r = runAot(ownedPayloadProgram("Fetched(move response = var r)"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "`move` is not a slot spelling"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "write the positional destructure"));
}

// --- Diagnostic 2: plain store of a borrow-only move-only arm binding ------

BEZEL_TEST(positional_pattern_named_slot_remedy_5938,
           spec_15_3_move_only_store_remedy_names_positional_slot_only) {
    auto r = runAot(moveOnlyPayloadProgram("Wrap(inner = var i)"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "(only a positional slot of a move-only payload receives the "
           "transfer at arm selection)"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "marked named slot"));
}

BEZEL_TEST(positional_pattern_named_slot_remedy_5938,
           spec_15_3_move_only_store_advised_positional_slot_runs) {
    auto r = runAotChecked(moveOnlyPayloadProgram("Wrap(var i)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "dispose\ndispose\n");
}

BEZEL_TEST(positional_pattern_named_slot_remedy_5938,
           spec_19_3_move_marker_on_move_only_named_slot_is_a_parse_error) {
    auto r = runAot(moveOnlyPayloadProgram("Wrap(move inner = var i)"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "`move` is not a slot spelling"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "write the positional destructure"));
}
