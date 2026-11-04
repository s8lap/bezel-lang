// Issue #7966 (spec §7.4 / §7.5, stdlib §2.15 / §2.5, v0.355) -- the
// LOWERING of a member read of a NULLABLE BUILT-IN VALUE.
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.355) --------------------
//
// spec §7.4 *Member access on nullable values*:
//   "Member access, method calls, and indexing on a nullable value are compile
//   errors unless the compiler has narrowed it to non-null"
//
// spec §7.5 *Null-conditional access (`?.`)*:
//   "`x?.M` returns null if `x` is null; otherwise it performs the access. The
//   result type is always nullable"
//
// stdlib §2.15:
//   "public record JsonOptions(Indented: bool, IndentWidth: int,
//   AllowTrailingCommas: bool, IgnoreUnknownFields: bool, MaxDepth: int);"
//
// --- THE DEFECT ------------------------------------------------------------
//
// MEASURED on the base (r30-c1 cac9332e), for a `JsonOptions?`:
//
//   Console.WriteLine(o?.MaxDepth ?? -1);
//     -> compiled with the internal #3079 warning ("the argument of this
//        `Console.WriteLine` failed to lower to a value, so the whole
//        statement emits nothing") and printed NOTHING.
//
//   var x: int? := o?.MaxDepth;
//     -> compiled with the internal #2434 warning ("lowerLocalDecl's
//        initializer expression failed to lower to a value for this local's
//        slot (degraded to a zero-initialized value)") and `x` read as
//        `null`, so `x ?? -1` answered the DEFAULT `-1` where §7.5 requires
//        the component's value -- a SILENT WRONG ANSWER behind exit 0.
//
// The cause: the `?.` READ of a `JsonOptions` component carried no TYPE at
// all (the `.` twin was untyped too -- `JsonOptions` is a compiler-known
// value record with no user TypeDecl, so the ordinary member-lookup path
// never found its components).  With no type, §7.5's "the result type is
// always nullable" was not observable and codegen had nothing to lower.
//
// The `DateTime?` / `TimeSpan?` twins were fine already (fix #7778), and
// the NARROWED `.` form was fine already (fix #7993, commit 7aaf129a).
// Rows 6-10 below are those CONTROLS: green on the base and on the fix.
//
// --- THE ORACLE ------------------------------------------------------------
//
// `runAot` -- the strict AOT oracle (fork-isolated `Driver::compileFile` +
// clang link + native exec).  Every value row asserts the EXACT stdout: a
// silent wrong value (the #2434 / #3079 degraded-slot shape) exits 0, so
// "it ran" is not an oracle.  ASan is NOT used: the fix touches no
// allocation, layout, retain/release or disposal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

// `module M; import System; import System.Serialization; class P ...`
std::string prog(const std::string& body) {
    return "module M; import System; import System.Serialization; "
           "class P public static method Main() " +
           body +
           "end Main; end P;";
}

// A USER-RECORD control: the same shapes, over a type the compiler knows
// nothing builtin about.  `record R(V: int);` is POSITIONAL, so §9.2 makes
// it auto-hashable and `r <> null` is available (a body record is not).
std::string userRecordProg(const std::string& body) {
    return "module M; import System; record R(V: int); "
           "class P public static method Main() " +
           body +
           "end Main; end P;";
}

// A `JsonOptions` with DISTINCT values in every component, so a mis-mapped
// component index changes the printed output.
const char* kOptions = "new JsonOptions(true, 4, false, true, 9)";

}  // namespace

// =====================================================================
// FIX ROW 1 -- the `?.` read of a component of a NON-NULL `JsonOptions?`
// prints the component's value (stdlib §2.15's `MaxDepth: int`).
//
// Pre-fix: the #3079 warning and NOTHING printed.
// Post-fix: "11".
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           jsonoptions_qmark_read_prints_the_component) {
    auto r = runAot(prog(
        "var o: JsonOptions? := new JsonOptions(true, 6, false, true, 11); "
        "Console.WriteLine(o?.MaxDepth ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n"));
}

// =====================================================================
// FIX ROW 2 -- §7.5's SHORT-CIRCUIT: "returns null if `x` is null".  A
// null receiver yields the `??` default, not the component's value.
//
// Pre-fix: the #3079 warning and NOTHING printed.
// Post-fix: "-1".
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           jsonoptions_qmark_read_of_a_null_receiver_prints_the_default) {
    auto r = runAot(prog(
        "var o: JsonOptions? := null; "
        "Console.WriteLine(o?.MaxDepth ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-1\n"));
}

// =====================================================================
// FIX ROW 3 -- the SILENT-WRONG-VALUE row.  The `?.` read assigned to a
// nullable local, then read back: §7.5's result is the component's value,
// so the `??` default must NOT be taken.
//
// Pre-fix: the #2434 warning ("degraded to a zero-initialized value") and
// "-1" -- the DEFAULT, where the component's value is 11.
// Post-fix: "11".
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           jsonoptions_qmark_read_assigned_to_a_nullable_local) {
    auto r = runAot(prog(
        "var o: JsonOptions? := new JsonOptions(true, 6, false, true, 11); "
        "var x: int? := o?.MaxDepth; "
        "Console.WriteLine(x ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n"));
}

// =====================================================================
// FIX ROW 4 -- a BOOL component (stdlib §2.15's `Indented: bool`).  The
// `?.` result is `bool?`, so the `??` default is the other bool.
//
// Pre-fix: the #3079 warning and NOTHING printed.
// Post-fix: "True".
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           jsonoptions_qmark_read_of_a_bool_component) {
    auto r = runAot(prog(
        "var o: JsonOptions? := new JsonOptions(true, 6, false, true, 11); "
        "Console.WriteLine(o?.Indented ?? false); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

// =====================================================================
// FIX ROW 5 -- all five components through the `?.` spelling, in
// stdlib §2.15's DECLARATION order.  The three boolean components cannot
// all carry distinct values (a bool has two), so the DISTINCTNESS that
// catches a mis-mapped index is spread over this row and the two int
// components carry distinct values (4 and 9) so a 1<->4 swap is caught too.
//
// Pre-fix: the #3079 warning and NOTHING printed.
// Post-fix: "True\n4\nFalse\nTrue\n9\n".
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           jsonoptions_qmark_read_of_all_five_components) {
    auto r = runAot(prog(
        "var o: JsonOptions? := new JsonOptions(true, 4, false, true, 9); "
        "Console.WriteLine(o?.Indented ?? false); "
        "Console.WriteLine(o?.IndentWidth ?? -1); "
        "Console.WriteLine(o?.AllowTrailingCommas ?? true); "
        "Console.WriteLine(o?.IgnoreUnknownFields ?? false); "
        "Console.WriteLine(o?.MaxDepth ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n4\nFalse\nTrue\n9\n"));
}

// =====================================================================
// CONTROL ROW 6 -- the NARROWED `.` form still prints (fix #7993's arm):
// `if o <> null then o.IndentWidth` -- §7.4's narrowing, then the `.`
// read.
//
// Green on the base and on the fix: "6".
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           jsonoptions_narrowed_dot_read_still_prints) {
    auto r = runAot(prog(
        "var o: JsonOptions? := new JsonOptions(true, 6, false, true, 11); "
        "if o <> null then Console.WriteLine(o.IndentWidth); end if; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

// =====================================================================
// CONTROL ROW 7 -- the `DateTime?` family's `?.` read still prints (fix
// #7778's arm): `d?.Year`.
//
// Green on the base and on the fix: "2024".
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           datetime_qmark_read_still_prints) {
    auto r = runAot(prog(
        "var d: DateTime? := new DateTime(2024, 3, 5); "
        "Console.WriteLine(d?.Year ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2024\n"));
}

// =====================================================================
// CONTROL ROW 8 -- the `TimeSpan?` family's `?.` read still prints (fix
// #7778's arm): `t?.TotalSeconds`.
//
// Green on the base and on the fix: "90".
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           timespan_qmark_read_still_prints) {
    auto r = runAot(prog(
        "var t: TimeSpan? := TimeSpan.FromSeconds(90); "
        "Console.WriteLine(t?.TotalSeconds ?? -1.0); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("90\n"));
}

// =====================================================================
// CONTROL ROW 9 -- the USER-RECORD control: the `?.` read of a record
// component still prints.  The fix is about the BUILT-IN value family, and
// this row is green on the base and on the fix: "7".
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           user_record_qmark_read_still_prints) {
    auto r = runAot(userRecordProg(
        "var r: R? := new R(7); "
        "Console.WriteLine(r?.V ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// =====================================================================
// CONTROL ROW 10 -- the USER-RECORD control's NARROWED `.` form still
// prints: "7".
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           user_record_narrowed_dot_read_still_prints) {
    auto r = runAot(userRecordProg(
        "var r: R? := new R(7); "
        "if r <> null then Console.WriteLine(r.V); end if; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// =====================================================================
// CONTROL ROW 11 -- the row that keeps the fix sound: the UN-NARROWED
// read of a nullable `JsonOptions?` local is still a compile error (spec
// §7.4), with the §7.4 diagnostic.
//
// Green on the base and on the fix: the base already rejected it, and the
// fix must not widen the unsound path.
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_lower_7966,
           jsonoptions_un_narrowed_read_is_still_rejected) {
    auto r = runAot(prog(
        "var o: JsonOptions? := null; "
        "Console.WriteLine(o.IndentWidth); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "member access `.IndentWidth` on a nullable value"));
}
