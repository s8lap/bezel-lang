// Regression for #7993 -- the component read of a NULLABLE `JsonOptions?`
// carrier did not lower, so the enclosing statement emitted nothing (the
// #3079 "the argument of this `Console.WriteLine` failed to lower to a value"
// backstop).
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ----------------------
//
// stdlib §2.15 (v0.350): `public record JsonOptions(Indented: bool,
//   IndentWidth: int, AllowTrailingCommas: bool, IgnoreUnknownFields: bool,
//   MaxDepth: int);` -- "All five components are required: a positional record
//   admits no default values (spec A.3 PositionalField), so `new
//   JsonOptions(...)` supplies every one."  A record component is readable as a
//   property.
//
// spec §7.4 (v0.350) *Member access on nullable values*: "Member access, method
//   calls, and indexing on a nullable value are compile errors unless the
//   compiler has narrowed it to non-null" -- and, in *How narrowing is
//   established*, "assignment: ... likewise after `x := expr;` where `expr`'s
//   static type is non-nullable ... and from a declaration whose initializer is
//   such an `expr` -- `var x: T? := expr;` -- which is the declaration's
//   assignment".  §7.2 (v0.350): "A value type wrapped in `?` is an optional:
//   it either holds a value or `null`."
//
// --- THE DEFECT ------------------------------------------------------------
//
// #7925 (stdlib §2.15, v0.350) lowered the five component reads of a BUILT-IN,
// NON-NULLABLE `JsonOptions` VALUE.  The NULLABLE form did not: the carrier is
// the tagged {i8 hasValue; bz.json_options} box, so `o.IndentWidth` over a
// `JsonOptions?` reached the read with the box still wrapped and lowered to
// nothing.  MEASURED on the base (r30-c1 2d33dc90): a NARROWED `JsonOptions?`
// local -- `if o <> null then Console.WriteLine(o.IndentWidth); end if;` --
// compiled with the #3079 warning and printed nothing.
//
// The fix is in the codegen `JsonOptions` arm (src/codegen/LlvmBackend_Exprs.cpp):
// the nullable carrier's payload is extracted before the component's
// `extractvalue`.  It is SOUND because sema's §7.4 member-access gate
// (ExprTypecheck_Calls.cpp's `resolveMemberAccess`) REJECTS the un-narrowed
// read of a nullable receiver -- for every receiver shape -- and codegen runs
// only when the program carries no errors; so a nullable receiver that reaches
// codegen is a §7.4-narrowed read.  Rows 5-7 below pin that rejection (the
// rows that keep the fix sound); rows 1-4 pin the narrowed read; row 8 is the
// non-nullable control.
//
// --- THE ORACLE ------------------------------------------------------------
//
// `runAot` -- the strict AOT oracle (fork-isolated `Driver::compileFile` +
// clang link + native exec).  ASan is NOT used: the fix touches no
// allocation, layout, retain/release or disposal -- it only reads a component
// out of a value the program already holds.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// `module M; import System; import System.Serialization; class P ...`
std::string prog(const std::string& body) {
    return "module M; import System; import System.Serialization; "
           "class P public static method Main() " +
           body +
           "end Main; end P;";
}

// The §7.4 rejection text the un-narrowed rows pin.  Pinned as a SUBSTRING of
// the driver's captured stderr, and only together with
// `failedAtCompileTimeWithRealLocation` -- so a runtime crash or a timeout
// cannot be mistaken for the rejection.
//
// §7.4's gate words the three receiver shapes differently ("... on a nullable
// value requires narrowing it to non-null first", "... on a nullable field or
// property requires narrowing to non-null first", "... on a nullable
// expression result requires narrowing to non-null first"), so the shared
// fragment is the tail ("non-null first (§7.4)") and each row additionally
// pins the receiver-shape fragment that names ITS shape.
const char* kNarrowingRequired = "non-null first (\xC2\xA7""7.4)";

// A narrowed `JsonOptions?` local: `var o: JsonOptions? := <value>;` then
// `if o <> null then ... end if;`.  The five components are read and printed
// in DECLARATION order (stdlib §2.15's record order), one per line.
//
// The three boolean components cannot all carry distinct values (a bool has
// two), so the DISTINCTNESS that catches a mis-mapped index is spread over two
// rows: `all_five_components_print` uses (Indented, AllowTrailingCommas,
// IgnoreUnknownFields) = (true, false, true) and
// `all_five_components_print_swapped_bools` uses (true, true, false).  Between
// them every permutation of the three boolean indices is caught (a 0<->2,
// 0<->3 or 2<->3 swap changes at least one row's printed triple), and the two
// int components carry distinct values in both rows (4 vs 9, 7 vs 3) so a
// 1<->4 swap is caught too.  An int<->bool confusion is caught by the printed
// value itself (an int prints a number, a bool prints True/False).
std::string fiveComponentBody(const char* indented, int width, const char* trailing,
                              const char* ignoreUnknown, int maxDepth) {
    return "var o: JsonOptions? := new JsonOptions(" +
           std::string(indented) + ", " + std::to_string(width) + ", " +
           std::string(trailing) + ", " + std::string(ignoreUnknown) + ", " +
           std::to_string(maxDepth) + "); "
           "if o <> null then "
           "Console.WriteLine(o.Indented); "
           "Console.WriteLine(o.IndentWidth); "
           "Console.WriteLine(o.AllowTrailingCommas); "
           "Console.WriteLine(o.IgnoreUnknownFields); "
           "Console.WriteLine(o.MaxDepth); "
           "end if; ";
}

}  // namespace

// FIX ROW.  The five component reads of a NARROWED `JsonOptions?` carrier each
// lower and print the right value.
BEZEL_TEST(jsonoptions_nullable_property_read_7993,
           all_five_components_print) {
    auto r = runAot(prog(fiveComponentBody("true", 4, "false", "true", 9)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n4\nFalse\nTrue\n9\n"));
}

// FIX ROW.  The complementary boolean triple, so that every permutation of the
// three boolean component indices is caught across the two rows.
BEZEL_TEST(jsonoptions_nullable_property_read_7993,
           all_five_components_print_swapped_bools) {
    auto r = runAot(prog(fiveComponentBody("true", 7, "true", "false", 3)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n7\nTrue\nFalse\n3\n"));
}

// FIX ROW.  The narrowing §7.4's *How narrowing is established* names for a
// DECLARATION: "from a declaration whose initializer is such an `expr` --
// `var x: T? := expr;` -- which is the declaration's assignment".  The
// initializer here is the NON-NULLABLE `new JsonOptions(...)`, so the
// declaration narrows the carrier and the read is admitted -- and must lower.
BEZEL_TEST(jsonoptions_nullable_property_read_7993,
           declaration_initializer_narrows_and_reads) {
    auto r = runAot(prog(
        "var o: JsonOptions? := new JsonOptions(true, 4, false, true, 9); "
        "Console.WriteLine(o.IndentWidth); "
        "Console.WriteLine(o.MaxDepth); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n9\n"));
}

// FIX ROW.  The two other narrowing carriers: a FIELD copied to a local (the
// §7.4 remedy for a non-narrowable field -- "copy to a local first and narrow
// the local") and a BY-VALUE PARAMETER narrowed inside the method body.
BEZEL_TEST(jsonoptions_nullable_property_read_7993,
           field_copy_and_by_value_parameter) {
    auto r = runAot(
        "module M; import System; import System.Serialization; "
        "class Box public property Opt: JsonOptions?; end Box; "
        "class P "
        "  public static method Width(o: JsonOptions?): int "
        "    if o <> null then return o.IndentWidth; end if; "
        "    return 0; "
        "  end Width; "
        "  public static method Main() "
        "    var b := new Box(); "
        "    b.Opt := new JsonOptions(true, 4, false, true, 9); "
        "    var o: JsonOptions? := b.Opt; "
        "    if o <> null then Console.WriteLine(o.IndentWidth); end if; "
        "    Console.WriteLine(Width(b.Opt)); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n4\n"));
}

// FIX ROW.  The components are also read as OPERANDS, not only printed: a
// mis-mapped index would change the sum, and a mis-TYPED extract would not
// even verify.
BEZEL_TEST(jsonoptions_nullable_property_read_7993,
           components_as_operands) {
    auto r = runAot(prog(
        "var o: JsonOptions? := new JsonOptions(true, 4, false, true, 9); "
        "if o <> null then "
        "var d: int := o.IndentWidth + o.MaxDepth; "
        "Console.WriteLine(d); "
        "if o.AllowTrailingCommas and not o.Indented then "
        "Console.WriteLine(\"ok\"); "
        "end if; "
        "end if; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("13\n"));
}

// CONTROL ROW -- the row that keeps the fix sound.  An UN-NARROWED read of a
// nullable `JsonOptions?` LOCAL is still a compile error (spec §7.4), with the
// §7.4 diagnostic.  Green on the base and on the fix: the base already
// rejected it, and the fix must not widen the unsound path.
BEZEL_TEST(jsonoptions_nullable_property_read_7993,
           un_narrowed_local_read_is_rejected) {
    auto r = runAot(prog(
        "var o: JsonOptions? := null; "
        "Console.WriteLine(o.IndentWidth); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNarrowingRequired));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "member access `.IndentWidth` on a nullable value"));
}

// CONTROL ROW -- the same rule for a FIELD / PROPERTY receiver, which §7.4
// never narrows (the TOCTOU rule): "copy the nullable value to a local
// variable and guard with `<> null`".
BEZEL_TEST(jsonoptions_nullable_property_read_7993,
           un_narrowed_field_read_is_rejected) {
    auto r = runAot(
        "module M; import System; import System.Serialization; "
        "class Box public property Opt: JsonOptions?; end Box; "
        "class P public static method Main() "
        "  var b := new Box(); "
        "  Console.WriteLine(b.Opt.IndentWidth); "
        "end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNarrowingRequired));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "member access `.IndentWidth` on a nullable field or property"));
}

// CONTROL ROW -- and for a CALL RESULT receiver, which is never narrowable
// either (no local slot to pin the fact to).
BEZEL_TEST(jsonoptions_nullable_property_read_7993,
           un_narrowed_call_result_read_is_rejected) {
    auto r = runAot(
        "module M; import System; import System.Serialization; "
        "class P "
        "  public static method Get(): JsonOptions? => null; "
        "  public static method Main() "
        "    Console.WriteLine(Get().IndentWidth); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNarrowingRequired));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "member access `.IndentWidth` on a nullable expression result"));
}

// CONTROL ROW -- the non-nullable form (#7925) still works: the fix must not
// disturb the bare `bz.json_options` value's read.
BEZEL_TEST(jsonoptions_nullable_property_read_7993,
           non_nullable_form_still_works) {
    auto r = runAot(prog(
        "var o := new JsonOptions(true, 4, false, true, 9); "
        "Console.WriteLine(o.Indented); "
        "Console.WriteLine(o.IndentWidth); "
        "Console.WriteLine(o.AllowTrailingCommas); "
        "Console.WriteLine(o.IgnoreUnknownFields); "
        "Console.WriteLine(o.MaxDepth); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n4\nFalse\nTrue\n9\n"));
}
