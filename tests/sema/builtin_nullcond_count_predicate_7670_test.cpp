// Spec-derived regression tests for issue #7670: `?.Count(pred)` on a receiver
// that declares `Count` (§26.5 rule 1).
//
// Spec §26.5 rule 1 (v0.350): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` — of any arity, applicability,
// or accessibility — instance resolution is committed and extension blocks
// are **never consulted** for that name on that receiver. If the committed
// instance lookup then fails ..., the call is a compile error citing the
// instance member(s); it does not fall back to an extension."
// Stdlib §4.4 (v0.350) declares the property `Count` on `Array<T>`; stdlib §1
// "Block-by-name" says `arr.Count(pred)` does not resolve to the predicate
// count on an `Array<T>`; §4.1 gives `IList<T>` / `Set<T>` the inherited
// `Count`. The `.` spelling refuses `a.Count(pred)` (#3342, #7597); the `?.`
// spelling reached no check, so the call was left untyped and the local
// degraded to zero behind an `internal:` warning (#2434):
//
//   var l: IList<int>? := a;
//   var n := l?.Count(lambda(x) => x > 1) ?? -1;   // printed 0 behind the #2434 warning
//
// The ORACLE is the specific diagnostic; the CONTROLS (the arity-0 property,
// and the §5 predicate count on `IEnumerable<T>`, which declares no `Count`)
// must stay accepted.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& ext, const std::string& body) {
    return "module M;\nimport System.Linq;\n" + ext +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    var n: Array<int>? := a;\n"
           "    var z: Array<int>? := null;\n"
           "    var l: IList<int>? := a;\n"
           "    var st: Set<int> := [1,2,3];\n"
           "    var sn: Set<int>? := st;\n"
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

const char kArityNeedle[] = "The instance member takes no arguments, but this "
                            "call supplies 1";

}  // namespace

// ── #7670: `?.Count(pred)` on a receiver that declares `Count` ────────────

// The issue's repro on the concrete head.
BEZEL_TEST(sema_builtin_nullcond_count_predicate_7670,
           array_nullcond_count_predicate_rejected) {
    // per §26.5 rule 1 (v0.350) + stdlib §1 "Block-by-name", §4.4
    expectRejected("",
                   "var c := n?.Count(lambda(x) => x > 1);",
                   kArityNeedle);
}

// The issue's repro on the interface head (`IList<T>` inherits `Count`).
BEZEL_TEST(sema_builtin_nullcond_count_predicate_7670,
           ilist_nullcond_count_predicate_rejected) {
    // per §26.5 rule 1 (v0.350) + stdlib §1 "Block-by-name", §4.1
    expectRejected("",
                   "var c := l?.Count(lambda(x) => x > 1);",
                   kArityNeedle);
}

BEZEL_TEST(sema_builtin_nullcond_count_predicate_7670,
           set_nullcond_count_predicate_rejected) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.1, §4.10
    expectRejected("",
                   "var c := sn?.Count(lambda(x) => x > 1);",
                   kArityNeedle);
}

// A same-named user extension does not make the call reachable: rule 1
// commits by name "of any arity, applicability".
BEZEL_TEST(sema_builtin_nullcond_count_predicate_7670,
           array_nullcond_count_predicate_extension_does_not_rescue) {
    // per §26.5 rule 1 (v0.350)
    expectRejected("extension Array<int>\n"
                   "  public method Count(p: (int) -> bool): int => 42;\n"
                   "end;\n",
                   "var c := n?.Count(lambda(x) => x > 1);",
                   kArityNeedle);
}

// CONTROL: the arity-0 property under `?.` is the instance member and keeps
// working, including the null receiver.
BEZEL_TEST(sema_builtin_nullcond_count_predicate_7670,
           control_nullcond_count_property_still_accepted) {
    // per §7.5 + stdlib §4.4
    expectStdout("",
                 "Console.WriteLine(n?.Count ?? -1);\n"
                 "    Console.WriteLine(z?.Count ?? -1);",
                 "3\n-1\n");
}

// CONTROL: `IEnumerable<T>` declares no `Count`, so the §5 predicate count
// stays reachable under `?.`.
BEZEL_TEST(sema_builtin_nullcond_count_predicate_7670,
           control_nullcond_count_predicate_on_ienumerable_accepted) {
    // per stdlib §4.12 + §5
    expectStdout("",
                 "var e: IEnumerable<int>? := a;\n"
                 "    Console.WriteLine(e?.Count(lambda(x) => x > 1) ?? -1);",
                 "2\n");
}
