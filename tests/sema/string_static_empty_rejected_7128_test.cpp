// fix #7128 — `string.Empty` is ACCEPTED by sema, the member does not exist in
// stdlib §9.6, and the read spelling produces a program that dies with
// SIGSEGV.
//
// THE DEFECT. stdlib §9.6's `String` listing declares `Length` / `IsEmpty` /
// `IsAscii` and the construction methods (`From` / `FromCStringPointer` /
// `FromWStringPointer` / `Repeat` / `Join`) — and NO `Empty`, neither as a
// property nor as a method. `Bezel_For_Code_Generation.md` §5 lists
// `String.Empty` among the APIs that do NOT exist, with the replacement
// "use `""`". The compiler's string static-member accept set nevertheless
// carried `Empty`, so sema typed BOTH spellings — the member READ
// `string.Empty` and the CALL `string.Empty()` — as a `string`.
//
// THE CRASH. The CALL spelling lowers to the empty string literal and runs
// fine. The READ spelling has no lowering: the local it initialised was left
// zero-initialized (a NULL bz.string), and the program's first read of that
// local (`.Length`) dereferenced NULL at offset 0x8 — the string header's
// length field — so the GENERATED PROGRAM died with SIGSEGV (exit 139).
// MEASURED pre-fix on this tree: `var s := string.Empty; return s.Length;`
// COMPILED with exit 0 and the executable exited 139 (gdb: `mov 0x8,%eax`
// with the bz.string pointer in %rbx = 0x0). The compiler itself did not
// crash in the AOT path measured here; the crash was in the program it
// emitted. Either way the acceptance is the defect.
//
// THE REMEDY IS THE REJECTION, not an addition. Dropping `Empty` from the
// accept set (src/sema/ExprTypecheck_Calls.cpp) makes both spellings fall
// through to the clean §3.4 "type 'string' has no static method or property
// 'Empty'" reject — exactly as any other unknown static (`string.Bogus()`)
// and exactly as the `string.Format` / `string.Concat` removal (#1034)
// already does. Adding an `Empty` member would enlarge the surface against
// the normative listing and would need a stdlib amendment, which is out of
// scope for an implementation PR (docs/proposals/spec-change-process.md).
//
// THE CONTROLS ARE THE POINT. This is a SUBTRACTION, so the live risk is
// over-rejection of the members that STAYED: `Guid.Empty` (stdlib §2.6 — a
// DIFFERENT type's static sentinel), and every other member of the same
// accept set (`string.Join` / `string.From` / `string.Repeat` /
// `string.FromCStringPointer` / `string.FromWStringPointer`). Every control
// asserts a VALUE, never "it compiled" — a control that only proved the
// compiler did not error could not tell a preserved lowering from one that
// now returns the wrong thing.
//
// THE SPELLING ENUMERATION. `Empty` is reachable in more than one spelling,
// and this file pins every one of them:
//   * `string.Empty`  — the static READ (the crashing one) — §3.4 reject.
//   * `string.Empty()` — the static CALL — §3.4 reject.
//   * `String.Empty` / `String.Empty()` — the §3.4 PascalCase alias of the
//     primitive keyword — §3.4 reject (the diagnostic names `String`).
//   * `"".Empty` / `"".Empty()` — the INSTANCE spelling — rejected by the
//     instance-member surface, which never admitted `Empty` (a different
//     diagnostic, naming the member).
//   * `type S := string; S.Empty` — the TYPE-ALIAS spelling — also the
//     instance-member surface.
// The two instance-side spellings were already rejected before this fix; they
// are pinned here so that a future "add `String.Empty`" attempt (which would
// have to touch the instance accept list to be reachable) is caught here too.
//
// The one pre-existing lit row that pinned the OLD behaviour —
// tests/lit/sema/sema_unknown_primitive_method_spec3_4__string_empty_still_accepted_1034.bzl,
// which asserted `string.Empty` compiles clean — is INVERTED by this fix (it
// became ..._string_empty_is_rejected_7128.bzl).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A bare `module M; class C ... end C;` shell — the string statics are
// compiler-intrinsic, so no imports or hierarchy are required.
std::string prog(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// True iff SOME error diagnostic carries the §3.4 unknown-static wording for
// the qualified name `<recv>.<member>`.
bool unknownStatic(const JitResult& r, const std::string& recv,
                   const std::string& member) {
    const std::string needle = "type '" + recv +
                             "' has no static method or property '" +
                             member + "' (§3.4)";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// True iff SOME error diagnostic names the member `Empty` — the instance-side
// rejections, whose wording is the instance-member surface's, not §3.4's.
// The member NAME is what this file pins for those rows; the exact sentence
// belongs to the instance-member accept list, which is not this issue's
// surface.
bool namesEmptyMember(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("`.Empty`") != std::string::npos)
            return true;
    return false;
}

// The harness labels a COMPILER crash distinctly ("runAot: compiler CRASHED
// (signal N)") so triage can tell a front-end/codegen bug from an ordinary
// program rejection. A row that only asserted the §3.4 diagnostic would still
// pass if the compiler emitted that diagnostic and THEN crashed; this is the
// explicit no-crash oracle.
bool compilerCrashed(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("compiler CRASHED") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── REJECT — the static READ spelling: the member that does not exist ─────
// MEASURED pre-fix: accepted, typed `string`, and the program it produced
// dereferenced NULL when it read the value.
BEZEL_TEST(sema_string_static_empty_7128, reject_static_read) {
    auto r = runAot(prog("    var s: string := string.Empty;\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(unknownStatic(r, "string", "Empty"));
    BEZEL_EXPECT_FALSE(compilerCrashed(r));
}

// ── REJECT — the static CALL spelling: same member, other syntax ──────────
// MEASURED pre-fix: accepted, and lowered to the empty string.
BEZEL_TEST(sema_string_static_empty_7128, reject_static_call) {
    auto r = runAot(prog("    var s: string := string.Empty();\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(unknownStatic(r, "string", "Empty"));
    BEZEL_EXPECT_FALSE(compilerCrashed(r));
}

// ── REJECT — the §3.4 PascalCase alias of the primitive keyword ───────────
// §3.4's dual-form table pairs `string` with `System.String`'s alias `String`;
// the alias reaches the same primitive-static surface, so it must be rejected
// the same way (the diagnostic names the receiver the user wrote).
BEZEL_TEST(sema_string_static_empty_7128, reject_pascal_read) {
    auto r = runAot(prog("    var s: string := String.Empty;\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(unknownStatic(r, "String", "Empty"));
    BEZEL_EXPECT_FALSE(compilerCrashed(r));
}

BEZEL_TEST(sema_string_static_empty_7128, reject_pascal_call) {
    auto r = runAot(prog("    var s: string := String.Empty();\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(unknownStatic(r, "String", "Empty"));
    BEZEL_EXPECT_FALSE(compilerCrashed(r));
}

// ── REJECT — the issue's own reproducer, and the CRASH oracle ─────────────
// This is the program that produced the SIGSEGV: the read spelling initialised
// the local to a NULL bz.string and the `.Length` read dereferenced it at
// offset 0x8. Pre-fix this row COMPILED (exit 0) and the executable exited
// 139; post-fix the program is rejected at compile time and never runs.
BEZEL_TEST(sema_string_static_empty_7128, reject_crash_repro_no_crash) {
    auto r = runAot(prog("    var s := string.Empty;\n    return s.Length;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(unknownStatic(r, "string", "Empty"));
    BEZEL_EXPECT_FALSE(compilerCrashed(r));
}

// ── REJECT — the member used as an argument (a third context) ─────────────
BEZEL_TEST(sema_string_static_empty_7128, reject_as_argument) {
    auto r = runAot(prog("    Console.WriteLine(string.Empty);\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(unknownStatic(r, "string", "Empty"));
    BEZEL_EXPECT_FALSE(compilerCrashed(r));
}

// ── REJECT — the INSTANCE spellings ───────────────────────────────────────
// The instance-member surface never admitted `Empty`: `"".Empty` was already
// rejected. Pinned so a future attempt to ADD `String.Empty` (which would
// have to reach the instance accept list to be reachable) is caught here.
BEZEL_TEST(sema_string_static_empty_7128, reject_instance_read) {
    auto r = runAot(prog("    var s: string := \"\".Empty;\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(namesEmptyMember(r));
}

BEZEL_TEST(sema_string_static_empty_7128, reject_instance_call) {
    auto r = runAot(prog("    var s: string := \"\".Empty();\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(namesEmptyMember(r));
}

// ── REJECT — the TYPE-ALIAS spelling ──────────────────────────────────────
// `type S := string;` names a user type whose underlying type is `string`, so
// `S.Empty` lands on the instance-member surface, not the primitive-static
// one. Pinned for the same reason as the two instance rows above.
BEZEL_TEST(sema_string_static_empty_7128, reject_type_alias) {
    auto r = runAot("module M;\ntype S := string;\nclass C\n"
                    "  public static method Main(): int\n"
                    "    var s: string := S.Empty;\n    return 0;\n"
                    "  end Main;\nend C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(namesEmptyMember(r));
}

// ── ACCEPT-CONTROLS — the members that STAYED ─────────────────────────────
// Every control asserts a VALUE, so a control that stopped lowering would
// fail here rather than silently returning something else.

// stdlib §2.6: `Guid.Empty` — the all-zero Guid, a `static readonly
// property` on a DIFFERENT type. The contrast the issue draws: a sentinel of
// that shape is declared in the stdlib, `String.Empty` is not.
BEZEL_TEST(sema_string_static_empty_7128, accept_guid_empty) {
    auto r = runAot(prog(
        "    var g: Guid := Guid.Empty;\n"
        "    Console.WriteLine(g.ToString());\n    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var g: Guid := Guid.Empty;\n"
        "    Console.WriteLine(g.ToString());\n    return 0;")),
        "00000000-0000-0000-0000-000000000000");
}

// stdlib §9.6: `string.Join(separator, items)`.
BEZEL_TEST(sema_string_static_empty_7128, accept_string_join) {
    const std::string src = prog(
        "    using var items := new Array<string>();\n"
        "    items.Add(\"a\"); items.Add(\"b\");\n"
        "    Console.WriteLine(string.Join(\",\", items));\n    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "a,b");
}

// stdlib §9.6: `string.From(value: char)`.
BEZEL_TEST(sema_string_static_empty_7128, accept_string_from) {
    const std::string src = prog("    Console.WriteLine(string.From('x'));\n    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "x");
}

// stdlib §9.6: `string.Repeat(c: char, count: int)` — the STATIC spelling.
BEZEL_TEST(sema_string_static_empty_7128, accept_string_repeat_static) {
    const std::string src = prog("    Console.WriteLine(string.Repeat('x', 3));\n    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "xxx");
}

// stdlib §9.6 / §10.4: `string.FromCStringPointer(p: IntPtr)` — the
// spec's own FFI read-back idiom (§32.6's `getenv` example).
BEZEL_TEST(sema_string_static_empty_7128, accept_string_from_cstring_pointer) {
    const std::string src = prog(
        "    Console.WriteLine("
        "string.FromCStringPointer(CString.From(\"hi\").Pointer));\n    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "hi");
}

// stdlib §9.6 / §10.4: `string.FromWStringPointer(p: IntPtr)`.
BEZEL_TEST(sema_string_static_empty_7128, accept_string_from_wstring_pointer) {
    const std::string src = prog(
        "    Console.WriteLine("
        "string.FromWStringPointer(WString.From(\"hi\").Pointer));\n    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "hi");
}

// stdlib §9.6: the INSTANCE `Repeat` (the compiler's own surface) — pinned
// so the subtraction above cannot have removed it by accident.
BEZEL_TEST(sema_string_static_empty_7128, accept_instance_repeat) {
    const std::string src = prog("    Console.WriteLine(\"x\".Repeat(3));\n    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "xxx");
}
