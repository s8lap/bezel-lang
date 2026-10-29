// Spec-derived regression test for issue #7706.
//
// Stdlib §9.6 (v0.350) declares `Repeat` on `String` ONLY as a static member:
//
//   public static pure method Repeat(c: char, count: int): string;
//   public static pure method Repeat(unit: string, count: int): string;
//
// and stdlib §1 (v0.350) makes the listing normative for the type's names and
// signatures ("Implementations may add internal helper methods and private
// members; anything documented here is part of the public contract" -- an
// instance member a program can call is neither). The instance spelling
// `s.Repeat(2)` is therefore not a member of `string`.
//
// THE DEFECT. resolveMemberAccess typed the INSTANCE `Repeat` from its NAME
// (the CG27f string-method result list), codegen lowered it, and
// `var s := "abc"; Console.WriteLine(s.Repeat(2));` compiled and printed
// `abcabc`. Every other static-only §9.6 name (`Join`, `From`,
// `IsNullOrWhiteSpace`, ...) called through an instance draws "string member
// `.X` is not yet supported in codegen"; `Repeat` was the one admitted.
//
// ORACLE: the SPECIFIC diagnostic at a real source location on the rejection
// rows, exact stdout on the controls. The controls are load-bearing: the
// STATIC spelling must keep working, and -- §26.5 rule 2, since `string`
// declares no INSTANCE `Repeat` -- a user `extension string` named `Repeat`
// binds.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& ext, const std::string& body) {
    return "module M;\n" + ext +
           "class C\n"
           "  public static method Main(): int\n"
           "    var s: string := \"abc\";\n"
           "    var xs: Array<string> := [\"a\", \"b\"];\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void expectRejected(const std::string& body, const std::string& needle) {
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

void expectStdout(const std::string& ext, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char kRepeat[] = "string member `.Repeat` is not yet supported in codegen";

}  // namespace

// ── the DEFECT: the instance spelling of a static-only member ─────────────

// The issue's repro, verbatim: a local receiver.
BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           local_receiver_instance_repeat_rejected) {
    // per stdlib §9.6 + §1 (v0.350)
    expectRejected("Console.WriteLine(s.Repeat(2));", kRepeat);
}

// A literal receiver, bound to a local.
BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           literal_receiver_instance_repeat_rejected) {
    // per stdlib §9.6 + §1 (v0.350)
    expectRejected("var t: string := \"x\".Repeat(3);\n"
                   "    Console.WriteLine(t);", kRepeat);
}

// The char-argument shape of the static overload, through an instance.
BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           instance_with_static_argument_shape_rejected) {
    // per stdlib §9.6 (v0.350): `Repeat(c: char, count: int)` is static
    expectRejected("Console.WriteLine(s.Repeat('x', 2));", kRepeat);
}

// A method reference to the instance member (§27.6 forms it on a member the
// type declares; `string` declares no instance `Repeat`).
BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           bare_reference_rejected) {
    // per stdlib §9.6 (v0.350) + spec §27.6
    expectRejected("var f := s.Repeat;", kRepeat);
}

// The null-conditional spelling.
BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           null_conditional_instance_repeat_rejected) {
    // per stdlib §9.6 (v0.350)
    expectRejected("var t: string? := \"abc\";\n"
                   "    var r := t?.Repeat(2);", "`?.Repeat`");
}

// ── SIBLINGS: the other static-only §9.6 members are already rejected ─────
// Pinned so a future "admit by name" regression of any of them is caught.

BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           sibling_join_instance_rejected) {
    // per stdlib §9.6 (v0.350): `static Join(separator, items)`
    expectRejected("Console.WriteLine(s.Join(\",\", xs));",
                   "string member `.Join` is not yet supported");
}

BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           sibling_from_instance_rejected) {
    // per stdlib §9.6 (v0.350): `static From(value: char)`
    expectRejected("Console.WriteLine(s.From('c'));",
                   "string member `.From` is not yet supported");
}

BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           sibling_isnullorwhitespace_instance_rejected) {
    // per stdlib §9.6 (v0.350): `static IsNullOrWhiteSpace(value: string?)`
    expectRejected("var b := s.IsNullOrWhiteSpace(s);\n"
                   "    Console.WriteLine(b);",
                   "string member `.IsNullOrWhiteSpace` is not yet supported");
}

// ── CONTROLS ──────────────────────────────────────────────────────────────

// The declared STATIC spelling, both overloads.
BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           control_static_repeat_char_and_string) {
    // per stdlib §9.6 (v0.350)
    expectStdout("", "Console.WriteLine(string.Repeat('x', 3));\n"
                     "    Console.WriteLine(String.Repeat(s, 2));",
                 "xxx\nabcabc\n");
}

// §26.5 rule 2: `string` declares no INSTANCE `Repeat`, so rule 1 does not
// commit and a user `extension string` member of that name is the candidate.
BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           control_user_extension_repeat_binds) {
    // per spec §26.5 rule 2 (v0.350) + stdlib §9.6
    expectStdout("extension string\n"
                 "  public method Repeat(n: int): string => \"EXT\";\n"
                 "end;\n",
                 "Console.WriteLine(s.Repeat(2));", "EXT\n");
}

// A declared INSTANCE member of the same family is untouched.
BEZEL_TEST(sema_string_instance_repeat_static_only_7706,
           control_declared_instance_padleft_still_works) {
    // per stdlib §9.6 (v0.350): `PadLeft(totalWidth: int)`
    expectStdout("", "Console.WriteLine(s.PadLeft(5));", "  abc\n");
}
