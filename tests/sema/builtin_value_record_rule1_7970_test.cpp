// Spec-derived regression test for issue #7970: the §26.5 rule-1 commitment on
// a built-in VALUE-RECORD receiver.
//
// Spec §26.5 rule 1 (v0.350, docs/Bezel_Language_Spec.md:6109): "If `x`'s
// declared type (or any base type) *declares* any instance member named `Foo`
// — of any arity, applicability, or accessibility — instance resolution is
// committed and extension blocks are **never consulted** for that name on that
// receiver. If the committed instance lookup then fails (no applicable
// overload, §22.5/§22.6, or the member is inaccessible, §13.1), the call is a
// compile error citing the instance member(s); it does not fall back to an
// extension (the diagnostic should note a same-named extension where one
// exists)."
//
// Stdlib §2.5 (v0.350) declares `DateTime.AddDays(days: double): DateTime`,
// `TimeSpan.CompareTo(other: TimeSpan): int` and the ARITY-0 property
// `TimeSpan.Ticks: long`; §2.6 declares `Guid`; §2.7 `Random`. The value
// records carry no TypeSymbol, so the ordinary member walk cannot see what
// they declare, and the extension blocks were consulted for a name the record
// DECLARES.
//
// THE DEFECTS (all measured on the pre-fix tree, `build/bezelc`).
//
// (1) An extension TYPED the call (probe p41):
//     `extension DateTime { AddDays(d: double): int => 4242 }` typed
//     `z.AddDays(1.0)` by the extension's `int` while codegen emitted the
//     built-in's own `DateTime` lowering under it — the program was rejected
//     with "no member `Ticks` on primitive type `int`", the extension's
//     `int` in the local's slot where the record's `DateTime` belonged.
//
// (2) A declared ARITY-0 member called WITH arguments degraded (probe p42):
//     `t.Ticks(1)` on a `TimeSpan` dropped the argument, degraded to a
//     zero-initialized value behind exit 0 (the #2434 warning) and printed
//     `0` — a value the record's own `Ticks` never had. Rule 1's "the
//     committed instance lookup then fails" case is a COMPILE ERROR, not a
//     degraded value and not a same-named extension.
//
// (3) A declared METHOD was hijacked by a same-named extension (probe p19):
//     `extension TimeSpan { CompareTo(o: TimeSpan): string => "EXT" }` typed
//     `t.CompareTo(t)` by the extension's `string` — the pre-fix program was
//     rejected with "`+` requires both operands to be `string`", the
//     extension's `string` in a local where the record's `int` belonged.
//
// The ORACLE is the exact stdout on the value rows and the specific
// diagnostic on the rejection row. The CONTROL rows are load-bearing: a name
// the receiver's declared type does NOT declare must still bind the user's
// extension (§26.5 rule 2), and the declared members must keep working.
//
// The probe named p43 in the issue report — an `extension FrozenArray<int> {
// Contains(x: int) }` typing `fa.Contains(1)` — is a MIS-MEASUREMENT, and
// the CONTROL row below says why: stdlib §4.19 declares `FrozenArray<T>` with
// `From`, `Length`, the indexer, `ToArray` and the destructor, and NO
// `Contains`, so the receiver declares no instance member of that name and
// rule 1 does not block the extension (§26.5 rule 2 admits it). The
// extension stays reachable, and the row pins that.
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
           "    var z: DateTime := DateTime.FromTicks(0L);\n"
           "    var t: TimeSpan := TimeSpan.FromTicks(7L);\n"
           "    var fa: FrozenArray<int> := FrozenArray.From([1,2,3]);\n"
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
// house wording of the interface-head (#7597) and `?.` (#7670) refusals.
const char kArityNeedle[] =
    "The instance member takes no arguments, but this call supplies 1";

}  // namespace

// ── #7970 (1): an extension must not TYPE a call the record DECLARES ─────

// Probe p41: `DateTime.AddDays(days: double): DateTime` is declared
// (stdlib §2.5), so rule 1 commits and the record's own member answers —
// the extension's `int` never types the call. The record's own value is the
// oracle: one day is 864000000000 ticks.
BEZEL_TEST(sema_builtin_value_record_rule1_7970,
           datetime_adddays_extension_does_not_type_the_call) {
    // per §26.5 rule 1 (v0.350) + stdlib §2.5
    expectStdout("extension DateTime\n"
                 "  public method AddDays(d: double): int => 4242;\n"
                 "end;\n",
                 "var d := z.AddDays(1.0);\n"
                 "    Console.WriteLine(d.Ticks);",
                 "864000000000\n");
}

// Probe p19: `TimeSpan.CompareTo(other: TimeSpan): int` is declared
// (stdlib §2.5), so the record's own member answers. The `+ 1` is the
// oracle: the built-in's `int` adds, the extension's `string` does not.
BEZEL_TEST(sema_builtin_value_record_rule1_7970,
           timespan_compareto_extension_does_not_type_the_call) {
    // per §26.5 rule 1 (v0.350) + stdlib §2.5
    expectStdout("extension TimeSpan\n"
                 "  public method CompareTo(o: TimeSpan): string => \"EXT\";\n"
                 "end;\n",
                 "var c := t.CompareTo(t);\n"
                 "    Console.WriteLine($\"{c + 1}\");",
                 "1\n");
}

// ── #7970 (2): a declared ARITY-0 member called WITH arguments ────────────

// Probe p42: `TimeSpan.Ticks` is an ARITY-0 property (stdlib §2.5), so the
// committed instance lookup FAILS on a call that supplies an argument —
// §26.5 rule 1's "compile error" case, never the extension and never a
// degraded zero behind the #2434 warning.
BEZEL_TEST(sema_builtin_value_record_rule1_7970,
           timespan_ticks_property_called_with_args_rejected) {
    // per §26.5 rule 1 (v0.350) + §22.6 + stdlib §2.5
    expectRejected("extension TimeSpan\n"
                   "  public method Ticks(k: int): string => \"EXT\";\n"
                   "end;\n",
                   "var x := t.Ticks(1);\n"
                   "    Console.WriteLine(x);",
                   kArityNeedle);
}

// ── CONTROLS: what rule 1 does NOT block ─────────────────────────────────

// A name the record does NOT declare is the user's extension's to answer
// (§26.5 rule 2): the commitment is by the DECLARED name, not by the head.
BEZEL_TEST(sema_builtin_value_record_rule1_7970,
           control_datetime_undeclared_name_extension_still_reachable) {
    // per §26.5 rule 2 (v0.350) + stdlib §2.5
    expectStdout("extension DateTime\n"
                 "  public method Twice(): int => 7;\n"
                 "end;\n",
                 "Console.WriteLine(z.Twice());",
                 "7\n");
}

// A STATIC member is not an instance member, so a same-named extension
// stays reachable on a value receiver (the static name itself is reached
// through the TYPE, §22.12).
BEZEL_TEST(sema_builtin_value_record_rule1_7970,
           control_datetime_static_name_extension_still_reachable) {
    // per §26.5 rule 1 (v0.350) — "an instance member" — + stdlib §2.5
    expectStdout("extension DateTime\n"
                 "  public method FromTicks(t: long): int => 7;\n"
                 "end;\n",
                 "Console.WriteLine(z.FromTicks(3L));",
                 "7\n");
}

// `TimeSpan` declares NO `Add`/`Subtract` — stdlib §2.5 spells the arithmetic
// as OPERATORS on the type — so an `Add` extension on it is not blocked by
// rule 1 and must still dispatch. This is the row that keeps the
// commitment table honest: it is a per-NAME commitment, not a per-HEAD one.
BEZEL_TEST(sema_builtin_value_record_rule1_7970,
           control_timespan_add_extension_still_reachable) {
    // per §26.5 rule 2 (v0.350) + stdlib §2.5
    expectStdout("extension TimeSpan\n"
                 "  public method Add(x: int): int => 5;\n"
                 "end;\n",
                 "Console.WriteLine(t.Add(3));",
                 "5\n");
}

// The record's own members keep working: the property reads and the
// declared methods.
BEZEL_TEST(sema_builtin_value_record_rule1_7970,
           control_datetime_members_still_work) {
    // per stdlib §2.5 + §22.6
    expectStdout("", "Console.WriteLine(z.Year);\n"
                     "    Console.WriteLine(z.Ticks);\n"
                     "    Console.WriteLine(t.Ticks);",
                     "1\n0\n7\n");
}

// §4.19: `FrozenArray<T>` declares `GetEnumerator`, `Length` and `ToArray`
// — and NO `Contains`. So a `Contains` extension on it is NOT blocked by
// rule 1: the receiver declares no instance member of that name. This is the
// row that DISPROVES the p43 probe of the issue report.
BEZEL_TEST(sema_builtin_value_record_rule1_7970,
           control_frozenarray_contains_extension_still_reachable) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.19
    expectStdout("extension FrozenArray<int>\n"
                 "  public method Contains(x: int): string => \"EXT\";\n"
                 "end;\n",
                 "Console.WriteLine(fa.Contains(1));",
                 "EXT\n");
}
