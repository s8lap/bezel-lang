// Spec-derived regression tests for issue #8047 and issue #7542: the `?.`
// spelling of the §26.5 rule-1 commitment for a built-in surface whose
// members the ordinary member walk cannot see.
//
// Spec §26.5 rule 1 (v0.350, docs/Bezel_Language_Spec.md): "If `x`'s
// declared type (or any base type) *declares* any instance member named
// `Foo` — of any arity, applicability, or accessibility — instance resolution
// is committed and extension blocks are **never consulted** for that name on
// that receiver. If the committed instance lookup then fails (no applicable
// overload, §22.5/§22.6, or the member is inaccessible, §13.1), the call is a
// compile error citing the instance member(s); it does not fall back to an
// extension."
//
// THE DEFECT. The `?.` spelling is a distinct AST node
// (`ast::NullCondMemberExpr`) with its own resolver
// (`ExprTC::resolveNullCondMember`), and its rule-1 gate asks only the
// collection-interface surfaces, the concrete collection heads,
// `StringBuilder` and the `Object` base. It does NOT ask the two builtin
// surfaces that carry no user TypeSymbol and so are invisible to the
// ordinary member walk:
//
//   * the by-name VALUE RECORDS (stdlib §2.5 `DateTime` / `TimeSpan` /
//     `DateOnly` / `TimeOnly`, §2.6 `Guid`, §2.7 `Random`) — issue #8047;
//   * the built-in `string` (stdlib §9.6) — issue #7542.
//
// The `.` spelling commits rule 1 for both (#7970 for the value records; the
// string's own intrinsic arms come before the extension lookup). MEASURED on
// the pre-fix tree, the `?.` spelling consulted the user's extension blocks
// for a name the receiver DECLARES:
//
//   extension DateTime public method AddDays(d: double): int => 4242; end;
//   var z: DateTime? := DateTime.FromTicks(0L);
//   Console.WriteLine((z?.AddDays(1.0) ?? DateTime.FromTicks(0L)).Ticks);
//     -> the program is REJECTED ("no member `Ticks` on primitive type `int`"):
//        the EXTENSION's `int` typed the call.
//
//   extension TimeSpan public method Ticks(k: int): string => "EXT"; end;
//   var t: TimeSpan? := TimeSpan.FromTicks(7L);
//   Console.WriteLine(t?.Ticks(1) ?? "NONE");
//     -> prints `EXT` (the extension's value); the ARITY-0 declared member
//        `TimeSpan.Ticks` (stdlib §2.5) is never consulted.
//
//   extension string public method Length(): int => 42; end;
//   var s: string? := "abc";
//   Console.WriteLine(s?.Length ?? -1);
//     -> prints `1`; the built-in's `Length` is `3`.
//
//   extension string public method Substring(i: int): string => "EXT"; end;
//   Console.WriteLine(s?.Substring(1) ?? "NONE");
//     -> prints `EXT`; the `.` spelling prints `bc`.
//
// THE ORACLES. The value rows assert the exact stdout of the DECLARED
// member's answer — the record's own `AddDays` is one day in ticks
// (`864000000000`), the built-in's `Length` of "abc" is `3`. The rejection
// rows assert the specific diagnostic. The CONTROLS are load-bearing: a name
// the receiver does NOT declare must still bind the user's extension
// (§26.5 rule 2), a STATIC name must still bind it (a static is not an
// instance member, §22.12), the `.` spelling must be untouched, and the
// record's own members must keep working.
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
           "    var z: DateTime? := DateTime.FromTicks(0L);\n"
           "    var t: TimeSpan? := TimeSpan.FromTicks(7L);\n"
           "    var s: string? := \"abc\";\n"
           "    var sn: string := \"abc\";\n"
           "    var sb: StringBuilder? := new StringBuilder();\n"
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

void expectRejected(const std::string& ext, const std::string& body,
                    const std::string& needle) {
    auto r = runAot(prog(ext, body));
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

// §26.5 rule 1's "the committed instance lookup then fails" wording — the
// house wording of the `.` spelling's refusal (#7970) and of the interface
// heads' (#7597) and `?.` (#7670) refusals.
const char kArityNeedle[] =
    "The instance member takes no arguments, but this call supplies 1";

}  // namespace

// ── #8047: the `?.` spelling on a built-in VALUE RECORD ───────────────────

// `DateTime.AddDays(days: double): DateTime` is declared (stdlib §2.5), so
// rule 1 commits the lookup and the RECORD's own member answers. The
// extension's `int` never types the call: the program runs and prints the
// record's own value — one day in ticks. (Pre-fix the extension typed the
// call and the program was rejected outright.)
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           datetime_nullcond_adddays_binds_the_instance_member) {
    // per §26.5 rule 1 (v0.350) + stdlib §2.5 + §7.5
    expectStdout("extension DateTime\n"
                 "  public method AddDays(d: double): int => 4242;\n"
                 "end;\n",
                 "var d := z?.AddDays(1.0);\n"
                 "    Console.WriteLine((d ?? DateTime.FromTicks(0L)).Ticks);",
                 "864000000000\n");
}

// `TimeSpan.Ticks` is an ARITY-0 property (stdlib §2.5), so the committed
// instance lookup FAILS on a call that supplies an argument — §26.5 rule 1's
// "compile error" case, never the extension and never a degraded value with
// the argument silently dropped. The `.` spelling refuses the same call
// (#7970); the `?.` spelling reached no check.
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           timespan_nullcond_ticks_property_called_with_args_rejected) {
    // per §26.5 rule 1 (v0.350) + §22.6 + stdlib §2.5
    expectRejected("extension TimeSpan\n"
                   "  public method Ticks(k: int): string => \"EXT\";\n"
                   "end;\n",
                   "var x := t?.Ticks(1);\n"
                   "    Console.WriteLine(x ?? \"NONE\");",
                   kArityNeedle);
}

// ── #7542: the `?.` spelling on the built-in `string` (stdlib §9.6) ────────

// `string` declares `Length: int` (stdlib §9.6), so the built-in's own
// member answers the `?.` read: "abc" has three code points. (Pre-fix the
// read printed `1` — the extension's dispatch, not the built-in's value.)
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           string_nullcond_length_binds_the_instance_member) {
    // per §26.5 rule 1 (v0.350) + stdlib §9.6
    expectStdout("extension string\n"
                 "  public method Length(): int => 42;\n"
                 "end;\n",
                 "Console.WriteLine(s?.Length ?? -1);",
                 "3\n");
}

// `string` declares `Substring(startIndex: int): string` (stdlib §9.6), so
// the extension of the same name is unreachable on a `string` receiver —
// rule 1 commits by NAME, of any arity.
//
// fix #7747 (§7.5, stdlib §9.6, v0.355): the `?.` spelling of a §9.6
// instance method is no longer refused ("string member `?.Substring` is not
// yet supported in codegen"), so the call is ANSWERED — by the BUILT-IN's
// member, not by the extension. The extension's "EXT" never appears: the
// row now pins the §9.6 answer ("abc".Substring(1) = "bc"), which is
// strictly stronger than the old "it is rejected" expectation — a lowering
// that answered with the extension's value would fail this row.
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           string_nullcond_substring_extension_does_not_type_the_call) {
    // per §26.5 rule 1 (v0.350) + stdlib §9.6 + §7.5
    expectStdout("extension string\n"
                 "  public method Substring(i: int): string => \"EXT\";\n"
                 "end;\n",
                 "Console.WriteLine(s?.Substring(1) ?? \"NONE\");",
                 "bc\n");
}

// `ToUpper(): string` is declared (stdlib §9.6), so the extension of the
// same name — whose `int` return type the pre-fix compiler typed the call
// with — is unreachable.
//
// fix #7747 (§7.5, stdlib §9.6, v0.355): the `?.` spelling is no longer
// refused, and the call is answered by the BUILT-IN's member — the §9.6
// answer ("abc".ToUpper() = "ABC"), never the extension's 7. The row pins
// that value, which is strictly stronger than the old "it is rejected"
// expectation.
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           string_nullcond_toupper_extension_does_not_type_the_call) {
    // per §26.5 rule 1 (v0.350) + stdlib §9.6 + §7.5
    expectStdout("extension string\n"
                 "  public method ToUpper(): int => 7;\n"
                 "end;\n",
                 "Console.WriteLine(s?.ToUpper() ?? \"NONE\");",
                 "ABC\n");
}

// ── #7945: the `?.` twin of the `.` spelling's StringBuilder refusal ──────

// `StringBuilder.Length` is an ARITY-0 property (stdlib §3.1), so a call
// that supplies arguments to it has no candidate: the committed instance
// lookup FAILS and §22.6 makes the call a compile error citing the instance
// member. The `.` spelling refuses it (#7775); the `?.` spelling reached no
// gate, dropped the argument and degraded the slot to zero behind the #2434
// warning.
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           stringbuilder_nullcond_length_property_called_with_args_rejected) {
    // per §26.5 rule 1 (v0.350) + §22.6 + stdlib §3.1
    expectRejected("",
                   "var n := sb?.Length(1);\n"
                   "    Console.WriteLine(n ?? -1);",
                   "The instance member is a property that takes no arguments");
}

// ── CONTROLS: what rule 1 does NOT block ─────────────────────────────────

// A name the record does NOT declare is the user's extension's to answer
// (§26.5 rule 2): the commitment is by the DECLARED name, not by the head.
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           control_nullcond_undeclared_name_binds_the_extension) {
    // per §26.5 rule 2 (v0.350) + stdlib §2.5
    expectStdout("extension DateTime\n"
                 "  public method Twice(): int => 7;\n"
                 "end;\n",
                 "Console.WriteLine(z?.Twice() ?? -1);",
                 "7\n");
}

// A STATIC member is not an instance member, so a same-named extension stays
// reachable on a value receiver (the static name itself is reached through
// the TYPE, §22.12).
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           control_nullcond_static_name_binds_the_extension) {
    // per §26.5 rule 1 (v0.350) — "an instance member" — + stdlib §2.5
    expectStdout("extension DateTime\n"
                 "  public method FromTicks(t: long): int => 7;\n"
                 "end;\n",
                 "Console.WriteLine(z?.FromTicks(3L) ?? -1);",
                 "7\n");
}

// `TimeSpan` declares NO `Add` — stdlib §2.5 spells the arithmetic as
// OPERATORS on the type — so an `Add` extension on it is not blocked by
// rule 1 and must still dispatch under `?.`.
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           control_nullcond_timespan_add_binds_the_extension) {
    // per §26.5 rule 2 (v0.350) + stdlib §2.5
    expectStdout("extension TimeSpan\n"
                 "  public method Add(x: int): int => 5;\n"
                 "end;\n",
                 "Console.WriteLine(t?.Add(3) ?? -1);",
                 "5\n");
}

// The same for `string`: a name §9.6 does not declare is the extension's.
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           control_nullcond_string_undeclared_name_binds_the_extension) {
    // per §26.5 rule 2 (v0.350) + stdlib §9.6
    expectStdout("extension string\n"
                 "  public method Frobnicate(): int => 42;\n"
                 "end;\n",
                 "Console.WriteLine(s?.Frobnicate() ?? -1);",
                 "42\n");
}

// The `.` spelling is untouched: the extension still binds a name the
// receiver does not declare.
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           control_plain_spelling_undeclared_name_binds_the_extension) {
    // per §26.5 rule 2 (v0.350) + stdlib §2.5
    expectStdout("extension DateTime\n"
                 "  public method Twice(): int => 7;\n"
                 "end;\n",
                 "Console.WriteLine(z.Twice());",
                 "7\n");
}

// The declared members keep working: the record's own property read under
// `?.`, and the built-in's own `Substring` under the `.` spelling.
BEZEL_TEST(sema_builtin_nullcond_surface_rule1_8047,
           control_declared_members_still_work) {
    // per stdlib §2.5 + §9.6 + §7.5
    expectStdout("", "Console.WriteLine(z?.Year ?? -1);\n"
                     "    Console.WriteLine(sn.Substring(1));",
                     "1\nbc\n");
}
