// Regression tests for #7435 (the same-variable leg) -- a use of a local
// variable after an explicit disposal that DOMINATES the use was detected but
// only WARNED about, so the program compiled and the use read a disposed value:
//
//   var d := new D(7);
//   d.Dispose();
//   return d.Ping();        // compiled; DisposedAccessException checked,
//                           // a fault unchecked
//
// Language spec §28.2 (v0.357) "Use after a dominating explicit disposal":
//   "A use of a local variable -- a read, a borrow taken from it, a member
//    access, an argument position, a consume -- after an explicit `Dispose()`
//    or direct `DisposeAsync()` call (§28.12) -- or a discard of the binding
//    (§17.13) -- on the same variable that dominates the use site on every
//    path is a **compile error**"
//   "A closure that captures the variable itself, rather than a holder of its
//    value, reads the variable when it is invoked (§27.5): its invocation
//    after the kill is a use of the variable under the first sentence, and a
//    reassignment between the kill and the invocation lifts it as it lifts
//    any other read of the variable."
//   "One use is exempt: a second `Dispose()` or `DisposeAsync()`, or a second
//    discard, of the same binding is the runtime no-op the carve-out above
//    defines and draws a compile-time warning"
//   "**The split is between the kills, never between the use forms**: under a
//    *dominating* kill every use in the list above is this error -- a
//    **consume** exactly as a read is, `move x` and an `owned` argument
//    position included, the repeated explicit disposal alone excepted"
//
// Root cause: the four arms of `MethodOwnership` that see the state
// (`checkRead`, `markMovedByName`, `checkTransitiveCapturesDisposed`) reported
// at `Severity::Warning`, the severity the rule had before v0.357; the plain
// `:=` member store never read its receiver; the invocation check knew a
// closure only through the local its literal initialised; a discard of an
// already disposed binding was walked as a read; and `await x.DisposeAsync()`
// read its own receiver after recording the kill.
//
// Every REJECT row was measured on the parent compiler COMPILING (and the
// program then faulting or reading a disposed value at run time), by the
// lane's matrix (see the commit body). The rows drive the real AOT path
// (`runAot`): a rejected program does not run, and the diagnostic is the
// error's own text.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `Run` is called 200 times so a program that is admitted is also a leak and
// a use-after-free probe under the generated-code sanitizer leg.
std::string prog(const std::string& body) {
    return "module M; import System;\n"
           "public class D\n"
           "  private _n: int;\n"
           "  public Tag: int;\n"
           "  public constructor(n: int)\n"
           "    _n := n;\n"
           "  end constructor;\n"
           "  public method Ping(): int => _n;\n"
           "  public property N: int => _n;\n"
           "end D;\n"
           "public class C\n"
           "  public static method Show(d: D): int => d.Ping();\n"
           "  public static method Take(owned d: D): int\n"
           "    using var k := move d;\n"
           "    return k.Ping();\n"
           "  end Take;\n"
           "  public static method Run(kill: bool): int\n" +
           body +
           "  end Run;\n"
           "  public static method Main(): int\n"
           "    var total := 0;\n"
           "    var i := 0;\n"
           "    while i < 200 do\n"
           "      total := total + C.Run(i < 0);\n"
           "      i := i + 1;\n"
           "    end while;\n"
           "    Console.WriteLine(total);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The read / member-access / argument form.
constexpr const char* kUse =
    "use of `d` after a dominating explicit Dispose() call — the disposal "
    "reaches this use on every path";
// The consume form.
constexpr const char* kConsume =
    "move from a local `d` already disposed at this point (§28.2) — the "
    "explicit disposal reaches this consume on every path";
// The closure-invocation form.
constexpr const char* kInvoke =
    "closure captures `d` which was disposed before this invocation";
constexpr const char* kInvokeIsUse =
    "the invocation of a closure over a variable is a use of that variable";
// The exempt repeated disposal.
constexpr const char* kAgain = "local `d` is disposed again here";

void expectRejected(const bezel::test::JitResult& r, const char* needle) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

bool hasDiag(const OwnershipResult& r, bezel::Severity sev,
             const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != sev) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool hasAnyDiag(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ===========================================================================
// (a) The use forms of §28.2's first sentence, each after `d.Dispose();`.
// ===========================================================================

// "a member access": a method call on the disposed variable.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_method_call_after_dominating_dispose_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    d.Dispose();\n"
                               "    return d.Ping();\n")),
                   kUse);
}

// "a member access": a property read.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_property_read_after_dominating_dispose_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    d.Dispose();\n"
                               "    return d.N;\n")),
                   kUse);
}

// "an argument position".
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_argument_after_dominating_dispose_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    d.Dispose();\n"
                               "    return C.Show(d);\n")),
                   kUse);
}

// "a borrow taken from it": an alias declared AFTER the kill.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_alias_taken_after_dominating_dispose_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    d.Dispose();\n"
                               "    var b := d;\n"
                               "    return b.Ping();\n")),
                   kUse);
}

// "a borrow taken from it": a bound method reference formed after the kill.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_method_reference_formed_after_dominating_dispose_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    d.Dispose();\n"
                               "    var f := d.Ping;\n"
                               "    return f();\n")),
                   kUse);
}

// "a read" inside a string interpolation.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_interpolation_read_after_dominating_dispose_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    d.Dispose();\n"
                               "    var s := $\"{d.N}\";\n"
                               "    return s.Length;\n")),
                   kUse);
}

// "a member access" that STORES: the plain `:=` member store never read its
// receiver, so this compiled with no diagnostic at all.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_member_store_after_dominating_dispose_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    d.Dispose();\n"
                               "    d.Tag := 3;\n"
                               "    return 1;\n")),
                   kUse);
}

// "that dominates the use site on every path": the kill is in BOTH arms.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_kill_on_both_arms_dominates_the_read_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    if kill then d.Dispose(); else d.Dispose(); "
                               "end if;\n"
                               "    return d.Ping();\n")),
                   kUse);
}

// ===========================================================================
// (b) "or a discard of the binding (§17.13)" is the same kill.
// ===========================================================================

BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_read_after_dominating_discard_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    _ := d;\n"
                               "    return d.Ping();\n")),
                   kUse);
}

BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_member_store_after_dominating_discard_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    _ := d;\n"
                               "    d.Tag := 3;\n"
                               "    return 1;\n")),
                   kUse);
}

// ===========================================================================
// (c) "a consume exactly as a read is, `move x` and an `owned` argument
//     position included".
// ===========================================================================

BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_move_to_owned_argument_after_dominating_dispose_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    d.Dispose();\n"
                               "    return C.Take(move d);\n")),
                   kConsume);
}

BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_using_var_adoption_after_dominating_dispose_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    d.Dispose();\n"
                               "    using var k := move d;\n"
                               "    return 1;\n")),
                   kConsume);
}

// ===========================================================================
// (d) The closure over the VARIABLE: "its invocation after the kill is a use
//     of the variable under the first sentence".
// ===========================================================================

BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_closure_over_variable_invoked_after_kill_rejected) {
    auto r = runAot(prog("    var d := new D(7);\n"
                         "    var h := lambda() => d.Ping();\n"
                         "    d.Dispose();\n"
                         "    return h();\n"));
    expectRejected(r, kInvoke);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kInvokeIsUse));
}

// The closure is formed AFTER the kill and then invoked: the formation is not
// the use, the invocation is.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_closure_formed_after_kill_then_invoked_rejected) {
    auto r = runAot(prog("    var d := new D(7);\n"
                         "    d.Dispose();\n"
                         "    var h := lambda() => d.Ping();\n"
                         "    return h();\n"));
    expectRejected(r, kInvoke);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kInvokeIsUse));
}

// The kill is a discard.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_closure_over_variable_invoked_after_discard_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    var h := lambda() => d.Ping();\n"
                               "    _ := d;\n"
                               "    return h();\n")),
                   kInvoke);
}

// The closure reaches the invocation through a COPY of the function value
// (`var g := h;`): the parent drew no diagnostic at all here.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_copy_of_closure_over_variable_invoked_after_kill_rejected) {
    expectRejected(runAot(prog("    var d := new D(7);\n"
                               "    var h := lambda() => d.Ping();\n"
                               "    var g := h;\n"
                               "    d.Dispose();\n"
                               "    return g();\n")),
                   kInvoke);
}

// ===========================================================================
// (e) "an explicit `Dispose()` or direct `DisposeAsync()` call".
// ===========================================================================

namespace {
std::string asyncProg(const std::string& body) {
    return "module M; import System;\n"
           "public sealed class R: IAsyncDisposable\n"
           "  private _id: int;\n"
           "  public constructor(id: int)\n"
           "    _id := id;\n"
           "  end constructor;\n"
           "  public method Use(): int => _id;\n"
           "  public async method DisposeAsync(): Task\n"
           "  end DisposeAsync;\n"
           "end R;\n"
           "public class C\n"
           "  public static async method Body(kill: bool): Task<int>\n" +
           body +
           "  end Body;\n"
           "  public static method Main(): int\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}
}  // namespace

// The read after an awaited `DisposeAsync()` is the error, and it is reported
// AT THE READ. The parent reported the disposal call's own receiver instead
// (line 3, the `await` line) and said nothing at the read on line 4.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_read_after_awaited_dispose_async_rejected_at_the_read) {
    OwnershipResult r = parseAndCheckOwnership(asyncProg(
        "    var d := new R(1);\n"
        "    await d.DisposeAsync();\n"
        "    return d.Use();\n"));
    bool atRead = false;
    bool atDisposal = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(kUse) == std::string::npos) continue;
        if (d.severity == bezel::Severity::Error && d.location.line == 15)
            atRead = true;
        if (d.location.line == 14) atDisposal = true;
    }
    BEZEL_EXPECT_TRUE(atRead);
    BEZEL_EXPECT_FALSE(atDisposal);
}

// A lone awaited `DisposeAsync()` is a disposal, not a use after one: the
// parent's false *use of `d` after a dominating explicit Dispose()* warning at
// the call would be a false compile error under this rule.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_lone_awaited_dispose_async_draws_no_use_diagnostic) {
    OwnershipResult r = parseAndCheckOwnership(asyncProg(
        "    var d := new R(1);\n"
        "    await d.DisposeAsync();\n"
        "    return 1;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAnyDiag(r, "use of `d`"));
}

// "a second `Dispose()` or `DisposeAsync()` ... of the same binding" is the
// exempt repeat in BOTH spellings: a second awaited `DisposeAsync()` draws the
// *disposed again* warning. The parent gave both awaits the *use of* text
// (the exemption named `Dispose` only), which this rule makes an error on a
// legal program (rule-corpus row s28_12_0006 is this shape).
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_second_awaited_dispose_async_is_the_exempt_repeat_warning) {
    OwnershipResult r = parseAndCheckOwnership(asyncProg(
        "    var d := new R(1);\n"
        "    await d.DisposeAsync();\n"
        "    await d.DisposeAsync();\n"
        "    return 1;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, bezel::Severity::Warning, kAgain));
    BEZEL_EXPECT_FALSE(hasAnyDiag(r, "use of `d`"));
}

// ===========================================================================
// (f) "One use is exempt": the repeated disposal stays a WARNING, and it is
//     the *disposed again* warning, not the use diagnostic. A warning on a
//     program that compiles is invisible to `runAot`, so these two rows read
//     the diagnostics of the ownership pass directly.
// ===========================================================================

// A discard after a `Dispose()`: the parent walked the operand as a READ and
// gave it the *use of* text, which this rule makes an error.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_discard_after_dispose_is_the_exempt_repeat_warning) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var d := new D(7);\n"
        "    d.Dispose();\n"
        "    _ := d;\n"
        "    return 1;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, bezel::Severity::Warning, kAgain));
    BEZEL_EXPECT_FALSE(hasAnyDiag(r, "use of `d`"));
}

// "a second discard".
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_28_2_second_discard_is_the_exempt_repeat_warning) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var d := new D(7);\n"
        "    _ := d;\n"
        "    _ := d;\n"
        "    return 1;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, bezel::Severity::Warning, kAgain));
    BEZEL_EXPECT_FALSE(hasAnyDiag(r, "use of `d`"));
}

// ===========================================================================
// (g) The function local was REASSIGNED to a closure that captures nothing:
//     invoking it reads no disposed variable. The parent drew a false
//     "disposed before this invocation" warning here, which this rule would
//     have made a false compile error.
// ===========================================================================

BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           spec_27_5_reassigned_function_local_invoked_after_kill_is_clean) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var d := new D(7);\n"
        "    var h := lambda() => d.Ping();\n"
        "    h := lambda() => 5;\n"
        "    d.Dispose();\n"
        "    return h();\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAnyDiag(r, kInvoke));
}

// ===========================================================================
// The two named controls (base-green; every other admitted twin is a corpus
// candidate in the lane's probe directory).
// ===========================================================================

// CONTROL 1 -- "a reassignment between the kill and the invocation lifts it
// as it lifts any other read of the variable": a closure FORMED after the
// kill, the variable reassigned, then the closure invoked, is a legal program
// that reads the new value. Goes red under the plausible wrong fix that
// promotes the closure-FORMATION warning ("closure captures `d` after a
// dominating explicit Dispose() call") to the error along with the other
// three arms.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           control_closure_formed_after_kill_reassigned_then_invoked_runs) {
    auto r = runAotChecked(prog("    var d := new D(7);\n"
                                "    d.Dispose();\n"
                                "    var h := lambda() => d.Ping();\n"
                                "    d := new D(9);\n"
                                "    return h();\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1800\n"));
}

// CONTROL 2 -- the exempt repeated disposal compiles and runs: a second
// `Dispose()` and then a discard of the same binding. Goes red under the
// plausible wrong fix that promotes `checkRead`'s disposed arm without taking
// the discard of an already disposed binding out of the read walk.
BEZEL_TEST(ownership_use_after_dominating_dispose_is_error_7435,
           control_repeated_dispose_then_discard_compiles_and_runs) {
    auto r = runAotChecked(prog("    var d := new D(7);\n"
                                "    d.Dispose();\n"
                                "    d.Dispose();\n"
                                "    _ := d;\n"
                                "    return 1;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}
