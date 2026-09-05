// fix #3854 — delete the impl-ahead `String.IsNullOrEmpty` (stdlib §9.6,
// v0.290; proposal 0123 R6).
//
// WHAT v0.290 RETIRED. `String.IsNullOrEmpty(value: string?)` was accepted
// (and lowered in codegen) by the compiler but was never declared in stdlib
// §9.6's `String` member list — an impl-ahead-of-spec surface. Docs
// Bezel_Changelog.md's v0.290 entry records the retirement explicitly: "the
// spelling is `s = null or s.IsEmpty` (or `s?.IsEmpty ?? true`); spec §3.4's
// static-access example now reads `string.IsNullOrWhiteSpace`". This file
// pins the compiler catch-up: three call sites removed the surface (sema's
// closed-set static-member arm, `ExprTypecheck_Calls.cpp`; the registry row,
// `BuiltinRegistry.cpp`; the codegen lowering, `LlvmBackend_Calls2.cpp`).
//
// WHY THE DIAGNOSTIC TEXT MATTERS. `IsNullOrEmpty` and a typo of some other
// member both produce "no such member" from the compiler's point of view,
// but only one of them is worth re-reading source for. The reject rows below
// assert the REMOVAL wording (naming the real replacement) rather than
// `hasErrors()`, which cannot distinguish this rule from an unrelated
// unknown-member error.
//
// THE CONTROL IS THE POINT. `IsNullOrEmpty` shared its sema arm with
// `IsNullOrWhiteSpace` (both took the same `methodRef({string}, bool)`
// branch) — splitting it wrongly could have silently removed a documented
// member instead of the undocumented one. `accept_isnullorwhitespace_*` rows
// prove the sibling still works, in both call shapes (direct call and bare
// method-reference) that the shared arm used to serve.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

JitResult reject(const std::string& src) { return runAot(src); }

// True iff some ERROR diagnostic is the v0.290 removal wording for
// `String.IsNullOrEmpty`, naming the real replacement spelling.
bool removedIsNullOrEmpty(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        const std::string& m = d.message;
        if (m.find("'String.IsNullOrEmpty' was REMOVED from the standard "
                    "library at v0.290 (stdlib \xC2\xA7""9.6)") ==
            std::string::npos)
            continue;
        if (m.find("s = null or s.IsEmpty") == std::string::npos) continue;
        if (m.find("it is gone, not misspelled (issue 3854)") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

// The diagnostic must point at the call, never at 0:0.
//
// It cannot be checked through `d.location`: on a compile REJECT the AOT
// harness synthesizes exactly ONE diagnostic whose `location` is a
// default-constructed `SourceLocation` (0:0) and whose `message` is the
// whole captured stderr (`tests/support/aot_support.hpp`). The rendered
// `<file>:<line>:<col>:` prefix inside the message IS the real location and
// the one a user sees, so that is what this parses: two positive numbers
// immediately before the needle, separated by `:`.
bool locatedAt(const JitResult& r, const std::string& needle) {
    const std::string tail = ": " + needle;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const std::string& m = d.message;
        for (std::size_t at = m.find(tail); at != std::string::npos;
             at = m.find(tail, at + 1)) {
            std::size_t p = at;
            auto digitsBack = [&m](std::size_t& q) -> long {
                const std::size_t end = q;
                while (q > 0 && m[q - 1] >= '0' && m[q - 1] <= '9') --q;
                if (q == end) return -1;
                return std::stol(m.substr(q, end - q));
            };
            const long col = digitsBack(p);
            if (col <= 0 || p == 0 || m[p - 1] != ':') continue;
            --p;
            if (digitsBack(p) > 0) return true;
        }
    }
    return false;
}

}  // namespace

// ===========================================================================
// REJECT — `string.IsNullOrEmpty` as a direct call, and as a bare method
// reference (the two call shapes the shared sema arm serves, §27.6).
// ===========================================================================

BEZEL_TEST(sema_isnullorempty_removed_3854, reject_direct_call) {
    auto r = reject(prog("    var s: string? := \"hi\";\n"
                          "    if string.IsNullOrEmpty(s) then return 1; end;\n"
                          "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedIsNullOrEmpty(r));
    BEZEL_EXPECT_TRUE(locatedAt(r, "'String.IsNullOrEmpty' was REMOVED"));
}

BEZEL_TEST(sema_isnullorempty_removed_3854, reject_uppercase_string_alias) {
    // §3.4: `String` and `string` are interchangeable — the reject must not
    // depend on which spelling names the primitive.
    auto r = reject(prog("    var s: string? := \"hi\";\n"
                          "    if String.IsNullOrEmpty(s) then return 1; end;\n"
                          "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedIsNullOrEmpty(r));
}

BEZEL_TEST(sema_isnullorempty_removed_3854, reject_empty_string_literal_arg) {
    auto r = reject(prog("    if string.IsNullOrEmpty(\"\") then return 1; "
                          "end;\n    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedIsNullOrEmpty(r));
}

// §27.6: a primitive-intrinsic static method used as a bare method reference
// (not the direct callee of a call) is the OTHER branch `methodRef` used to
// serve for `IsNullOrEmpty` — a delegate value `(string) -> bool` passed to
// `Select`. This must reject exactly like the direct-call shape.
BEZEL_TEST(sema_isnullorempty_removed_3854, reject_bare_method_reference) {
    auto r = reject(
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<string?>();\n"
        "    a.Add(\"x\"); a.Add(null);\n"
        "    using var b := a.Select(string.IsNullOrEmpty).ToArray();\n"
        "    return b.Count;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedIsNullOrEmpty(r));
}

// ===========================================================================
// ACCEPT-CONTROLS — the sibling `IsNullOrWhiteSpace` that shared this sema
// arm must keep compiling AND keep its runtime value, in both call shapes.
// ===========================================================================

BEZEL_TEST(sema_isnullorempty_removed_3854,
           accept_isnullorwhitespace_direct_call_still_works) {
    // Non-null, non-whitespace -> false -> length branch: 5.
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var s: string? := \"hello\";\n"
                        "    if string.IsNullOrWhiteSpace(s) then "
                        "Console.WriteLine(0); else "
                        "Console.WriteLine(s.Length); end;\n"
                        "    return 0;")),
                    "5");
}

BEZEL_TEST(sema_isnullorempty_removed_3854,
           accept_isnullorwhitespace_null_arg_still_works) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var s: string? := null;\n"
                        "    Console.WriteLine((if "
                        "string.IsNullOrWhiteSpace(s) then 1 else 0));\n"
                        "    return 0;")),
                    "1");
}

BEZEL_TEST(sema_isnullorempty_removed_3854,
           accept_isnullorwhitespace_whitespace_arg_still_works) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine((if "
                        "string.IsNullOrWhiteSpace(\"   \") then 1 else 0));\n"
                        "    return 0;")),
                    "1");
}

BEZEL_TEST(sema_isnullorempty_removed_3854,
           accept_isnullorwhitespace_bare_reference_still_works) {
    // The bare-method-reference branch of the shared arm, kept for the
    // surviving sibling: `Select(string.IsNullOrWhiteSpace)`.
    BEZEL_EXPECT_EQ(runAotOut(
                        "module M;\n"
                        "import System;\n"
                        "class C\n"
                        "  public static method Main(): int\n"
                        "    using var a := new Array<string?>();\n"
                        "    a.Add(\"x\"); a.Add(null); a.Add(\"  \");\n"
                        "    using var b := "
                        "a.Select(string.IsNullOrWhiteSpace).ToArray();\n"
                        "    var n := 0;\n"
                        "    for var v in b do if v then n := n + 1; end; "
                        "end;\n"
                        "    Console.WriteLine(n);\n"
                        "    return 0;\n"
                        "  end Main;\n"
                        "end C;\n"),
                    "2");
}

// NOT covered here: whether `s = null or s.IsEmpty` (the replacement spelling
// this fix's diagnostic names, straight from the v0.290 changelog entry)
// itself compiles for a genuinely nullable `s`. Measured while writing this
// fix and deliberately NOT folded in, because it is not this: two SEPARATE,
// pre-existing gaps, filed as their own issues rather than silently worked
// around here (CLAUDE.md §4):
//   - `String.IsEmpty` (stdlib §9.6, documented since v0.231) has no codegen
//     lowering for a plain `string` receiver at all — `s.IsEmpty` on a
//     provably non-null `var s := "hello";` rejects with "string member
//     `.IsEmpty` is not yet supported in codegen", independent of nullability.
//   - (CLOSED by fix #3877.) The `or`-composition narrowing spec §7.4
//     documents ("in `A or B`, `B` sees `A`'s false-facts") did not fire for
//     an equality null-test: `s = null or s.Length` rejected "requires
//     narrowing" even for an unconstrained `string?` parameter, while the
//     dual `and`/`<>` form narrowed correctly. That half now works; only the
//     `String.IsEmpty` codegen gap above still blocks the documented
//     replacement idiom, so ONE of its two independent reasons remains.
// The diagnostic still names the spec-correct spelling (CLAUDE.md §1: the
// spec is the source of truth; a gap between spec and implementation is a
// separate defect, not a reason to change what the compiler tells the user
// to write).
