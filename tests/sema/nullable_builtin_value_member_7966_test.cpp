// Issue #7966 (spec §7.4 / §7.5, stdlib §2.15 / §2.5, v0.355) -- the member
// read of a NULLABLE BUILT-IN VALUE (`n.IndentWidth` on `JsonOptions?`,
// `d.Year` on `DateTime?`).
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
// --- WHAT THE ISSUE CLAIMED, AND WHAT IS TRUE ----------------------------
//
// The issue's TITLE claims two things.  MEASURED on the base (r30-c1
// cac9332e), the first does NOT hold and the second holds only in part:
//
//  * "an un-narrowed member read of a nullable built-in VALUE (`n.IndentWidth`
//    on `JsonOptions?`, `d.Year` on `DateTime?`) is accepted" -- it is NOT
//    accepted.  Sema's §7.4 gate (ExprTypecheck_Calls.cpp's
//    `resolveMemberAccess`) rejects it for `JsonOptions?`, `DateTime?` and
//    `TimeSpan?` alike, and with the SAME diagnostic a user record's
//    un-narrowed read draws -- "member access `.X` on a nullable value
//    requires narrowing it to non-null first (§7.4)".  Rows 1-5 below pin
//    that rejection and its TEXT for each receiver shape.  They are GREEN on
//    the base as well: they are the rows that keep the fix sound, not rows
//    that need it.
//
//  * "the narrowed and `?.` forms do not lower" -- the NARROWED `.` form
//    lowers (fix #7993, commit 7aaf129a, already on this branch); the `?.`
//    form did NOT lower for `JsonOptions?`, because the `?.` READ carried no
//    type at all: §7.5's "the result type is always nullable" was not
//    observable, and codegen had nothing to lower.  Row 7 is the RED row.
//
// --- THE ORACLE ------------------------------------------------------------
//
// `runAot` -- the strict AOT oracle (fork-isolated `Driver::compileFile` +
// clang link + native exec).  Every row here is a REJECTION row: it asserts
// that the program did not run and that the front end reported the SPECIFIC
// diagnostic.  ASan is NOT used: the fix touches no allocation, layout,
// retain/release or disposal.

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

// A user-record CONTROL program: the same §7.4 question, asked of a type
// the compiler knows nothing builtin about.  Its diagnostic is the house
// one -- the SAME wording the built-in value types draw -- which is what
// makes "reuse the existing text" checkable rather than merely asserted.
std::string userRecordProg(const std::string& body) {
    return "module M; import System; record R(V: int); "
           "class P public static method Main() " +
           body +
           "end Main; end P;";
}

// The §7.4 diagnostic's SHARED fragment.  §7.4's gate words the receiver
// shapes differently ("... on a nullable value requires narrowing it to
// non-null first", "... on a nullable field or property requires narrowing
// to non-null first", "... on a nullable expression result requires
// narrowing to non-null first"), so the shared fragment is the tail and each
// row additionally pins the fragment that names ITS shape.
const char* kNarrowingRequired = "non-null first (\xC2\xA7""7.4)";

// The §7.4 gate's wording for a SIMPLE VARIABLE receiver -- the shape a
// built-in value type shares with a user record.
const char* kNullableValueRequired =
    "on a nullable value requires narrowing it to non-null first";

// The row is a REJECTION: the program must not run, the front end must have
// reported an error at a real source location, and the error must carry the
// given text.
void expectRejected(const std::string& program, const char* needle) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// =====================================================================
// ROW 1 -- §7.4's gate on a `JsonOptions?` LOCAL.  Green on the base and
// on the fix: the base already rejected it, and the fix must not widen the
// unsound path.  The diagnostic TEXT is pinned, because a rename of it
// would be a behavior flip.
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_7966,
           jsonoptions_un_narrowed_local_read_is_rejected) {
    expectRejected(prog("var o: JsonOptions? := null; "
                        "Console.WriteLine(o.IndentWidth); "),
                   "member access `.IndentWidth` on a nullable value");
}

// =====================================================================
// ROW 2 -- the same gate on a `DateTime?` local (stdlib §2.5's
// `Year: int`).
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_7966,
           datetime_un_narrowed_local_read_is_rejected) {
    expectRejected(prog("var d: DateTime? := null; "
                        "Console.WriteLine(d.Year); "),
                   "member access `.Year` on a nullable value");
}

// =====================================================================
// ROW 3 -- the same gate on a `TimeSpan?` local (stdlib §2.5's
// `TotalSeconds: double`).
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_7966,
           timespan_un_narrowed_local_read_is_rejected) {
    expectRejected(prog("var t: TimeSpan? := null; "
                        "Console.WriteLine(t.TotalSeconds); "),
                   "member access `.TotalSeconds` on a nullable value");
}

// =====================================================================
// ROW 4 -- the USER-RECORD CONTROL: the same §7.4 question, asked of a
// type the compiler knows nothing builtin about, is refused with the SAME
// diagnostic.  This is the row that makes "the built-in value types reuse
// the user record's nullable-member diagnostic" a MEASURED claim rather
// than a claim about the text.
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_7966,
           user_record_un_narrowed_local_read_is_rejected) {
    expectRejected(userRecordProg("var r: R? := null; "
                                   "Console.WriteLine(r.V); "),
                   "member access `.V` on a nullable value");
}

// =====================================================================
// ROW 5 -- the FIELD / PROPERTY receiver shape (§7.4's TOCTOU rule: a
// field is never narrowable, so the read is refused with its own wording).
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_7966,
           jsonoptions_un_narrowed_field_read_is_rejected) {
    expectRejected("module M; import System; import System.Serialization; "
                   "class Box public property Opt: JsonOptions?; end Box; "
                   "class P public static method Main() "
                   "  var b := new Box(); "
                   "  Console.WriteLine(b.Opt.IndentWidth); "
                   "end Main; end P;",
                   "member access `.IndentWidth` on a nullable field or "
                   "property");
}

// =====================================================================
// ROW 6 -- the CALL-RESULT receiver shape (never narrowable: no local slot
// to pin the fact to).
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_7966,
           jsonoptions_un_narrowed_call_result_read_is_rejected) {
    expectRejected("module M; import System; import System.Serialization; "
                   "class P "
                   "  public static method Get(): JsonOptions? => null; "
                   "  public static method Main() "
                   "    Console.WriteLine(Get().IndentWidth); "
                   "  end Main; "
                   "end P;",
                   "member access `.IndentWidth` on a nullable expression "
                   "result");
}

// =====================================================================
// ROW 7 -- THE RED ROW.  §7.5: "The result type is always nullable."
// The `?.` read of a `JsonOptions` component carried NO TYPE at all (the
// `.` twin was untyped too), so §7.5's rule was not observable: a
// `JsonOptions?`'s `?.` component read could be assigned to a NON-nullable
// `int` without a word, and the program compiled.
//
// Pre-fix: the program compiled and ran (the right-hand side was untyped,
// so no conversion was checked) -- and the `?.` access itself lowered to
// nothing (#3079), so the program printed nothing.
// Post-fix: the `?.` read is `int?` (§7.5), and the assignment to a
// non-nullable `int` is refused with §7's nullable diagnostic.
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_7966,
           jsonoptions_qmark_read_is_always_nullable) {
    expectRejected(prog("var o: JsonOptions? := null; "
                        "var y: int := o?.MaxDepth; "
                        "Console.WriteLine(y); "),
                   "is nullable but the target type is non-nullable "
                   "(\xC2\xA7""7)");
}

// =====================================================================
// ROW 8 -- the CONTROL for row 7: the `DateTime?` family's `?.` read was
// ALREADY typed `int?` (fix #7778), so the same assignment is refused
// there.  This row is green on the base and on the fix, and it is what
// makes row 7 a fix of the JsonOptions family to the FAMILY's behaviour
// rather than a new rule.
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_7966,
           datetime_qmark_read_is_always_nullable) {
    expectRejected(prog("var d: DateTime? := null; "
                        "var y: int := d?.Year; "
                        "Console.WriteLine(y); "),
                   "is nullable but the target type is non-nullable "
                   "(\xC2\xA7""7)");
}

// =====================================================================
// ROW 9 -- the other half of the `?.` contract: the `?.` read DOES fit the
// NULLABLE form of its component's type (§7.5's "the result type is always
// nullable" is a statement about the type, not a refusal).  Green on the
// base (the untyped right-hand side converted to anything) and on the fix
// (the typed `int?` converts to `int?`).
// =====================================================================
BEZEL_TEST(nullable_builtin_value_member_7966,
           jsonoptions_qmark_read_fits_the_nullable_form) {
    JitResult r = runAot(prog("var o: JsonOptions? := null; "
                               "var y: int? := o?.MaxDepth; "
                               "Console.WriteLine(y ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-1\n"));
}
