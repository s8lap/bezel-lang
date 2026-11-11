// Regression tests for #7435 -- a use through a local ALIAS after a DOMINATING
// explicit disposal of the aliased variable was accepted, and the program then
// faulted on every run (`DisposedAccessException` in checked disposal mode, a
// heap-use-after-free under the generated-code ASan leg unchecked).
//
//   var c := new Customer("ann");
//   var b := c;                   // b is an ALIAS of c: a local borrow
//   c.Dispose();                  // the disposal DOMINATES the later use
//   Console.WriteLine(b.Name);    // a USE THROUGH THE ALIAS -- compile error
//
// Language spec §28.2 (v0.357) "Use after a dominating explicit disposal":
//   "A use of a local variable -- a read, a borrow taken from it, a member
//    access, an argument position, a consume -- after an explicit `Dispose()`
//    or direct `DisposeAsync()` call (§28.12) -- or a discard of the binding
//    (§17.13) -- on the same variable that dominates the use site on every
//    path is a **compile error** ... The same error reaches a use through a
//    local borrow of the variable's value: where a local holds a borrow of
//    that value -- an alias (`var b := a;`, `const b := a;`), ... -- taken
//    while the variable held that value, and every value in the holder's
//    source set (K2.2) has been so disposed on every path to the use, a use
//    of the holder -- a read or member access through it, passing it,
//    re-borrowing it, ... -- is the same compile error; the diagnostic names
//    the use, the holder, the disposed source and the disposing site. The
//    kill itself stays permitted (K5.3): what is rejected is the use the kill
//    makes certain to fault, and a disposal after the borrow's last use
//    changes no verdict. A reassignment of the variable between the kill and
//    the use does not lift the error -- the holder still borrows the disposed
//    value ... -- while a holder re-bound to the variable's new value
//    (`b := a;` after `a := new ...;`) takes a fresh borrow. ... A holder
//    whose source was disposed on only some paths to the use -- a kill at the
//    end of a loop body reaching the next iteration's use among them -- is
//    not rejected: a use through it raises `DisposedAccessException` in
//    checked disposal mode and is undefined behavior unchecked, the runtime
//    backstop above."
//
// Root cause: nothing asked, at a use of an alias, whether the value it
// borrows had been disposed. `checkRead`'s direct arm reads the disposed
// BINDING's own state, #7893 added the `using var` PIN holder only, and the
// plain alias was left to the runtime backstop.
//
// The fix records, on the HOLDER, which owner variable's value it borrowed and
// whether an explicit disposal of that value reaches the current point on
// every path (`LocalOwnership::heldBorrows`); the fact is set at the kill,
// AND-merged at a join, and read at every use of the holder.
//
// The rows are COMPILE-TIME (`parseAndCheckOwnership`): the defect is a
// compile-time over-acceptance. Every REJECT row below was measured faulting
// on the base compiler, and every ACCEPT row running clean, by the lane's
// matrix (see the commit body). The first REJECT row is the issue's own
// repro, byte for byte.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The diagnostic's stable fragment (§28.2), shared with the pin holder (#7893).
constexpr const char* kHolderUse =
    "after a dominating explicit Dispose() of";

// The §28.2 direct-shape diagnostic -- its severity is NOT moved by this fix.
constexpr const char* kDirectWarning =
    "use of `c` after a dominating explicit Dispose() call";

bool hasError(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool hasWarning(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool hasNote(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Note) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::size_t errorCount(const OwnershipResult& r, const std::string& needle) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            ++n;
    return n;
}

const std::string kHead =
    "module Probe;\n"
    "public class Customer\n"
    "  private _name: string;\n"
    "  public constructor(name: string)\n"
    "    _name := name;\n"
    "  end constructor;\n"
    "  public property Name: string => _name;\n"
    "  public method Hello(): int => 7;\n"
    "end Customer;\n"
    "public static class Sink\n"
    "  public static method Take(c: Customer): int => 1;\n"
    "end Sink;\n"
    "public class Program\n";

const std::string kBodyHead =
    "  public static method Body(k: int): int\n";

const std::string kTail =
    "    return 0;\n"
    "  end Body;\n"
    "  public static method Main(): int\n"
    "    return Body(1);\n"
    "  end Main;\n"
    "end Program;\n";

std::string prog(const std::string& body) {
    return kHead + kBodyHead + body + kTail;
}

}  // namespace

// ─────────────────────────────── REJECT ────────────────────────────────────

// REJECT: the issue's own repro (probe p07), byte for byte.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           issue_repro_alias_member_access_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        "module Probe;\n"
        "public class Customer\n"
        "  private _name: string;\n"
        "  public constructor(name: string)\n"
        "    _name := name;\n"
        "  end constructor;\n"
        "  public property Name: string => _name;\n"
        "end Customer;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    c.Dispose();\n"
        "    Console.WriteLine(b.Name);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    // "the diagnostic names the use, the holder, the disposed source and the
    // disposing site" (§28.2).
    BEZEL_EXPECT_TRUE(hasError(r, "use of `b`"));
    BEZEL_EXPECT_TRUE(hasError(r, "Dispose() of `c`"));
    BEZEL_EXPECT_TRUE(hasNote(r, "explicit Dispose() of `c` was here"));
    BEZEL_EXPECT_EQ(errorCount(r, kHolderUse), std::size_t{1});
}

// The spec's own example, verbatim (§28.3.1 K4, v0.357): "An explicit
// `Dispose()` while a borrow lives is permitted (K4.2). The flag catches a
// later use on the path where the kill ran (K4.1), and a use that the kill
// precedes on every path is §28.2's compile error". The FIRST use (through
// `b`, a conditional kill) is admitted; the SECOND (through `d`) is the error.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           spec_28_3_1_k4_example_conditional_admitted_dominating_rejected) {
    OwnershipResult r = parseAndCheckOwnership(
        "module Probe;\n"
        "public class Widget\n"
        "  private _name: string;\n"
        "  public constructor(name: string)\n"
        "    _name := name;\n"
        "  end constructor;\n"
        "  public property Name: string => _name;\n"
        "end Widget;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var flag := new Random().NextBoolean();\n"
        "    var a := new Widget(\"a\");\n"
        "    var b := a;\n"
        "    if flag then\n"
        "      a.Dispose();\n"
        "    end;\n"
        "    Console.WriteLine(b.Name);\n"
        "    var c := new Widget(\"c\");\n"
        "    var d := c;\n"
        "    c.Dispose();\n"
        "    Console.WriteLine(d.Name);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(hasError(r, "use of `d` after a dominating explicit "
                                  "Dispose() of `c`"));
    BEZEL_EXPECT_FALSE(hasError(r, "use of `b`"));
    BEZEL_EXPECT_EQ(errorCount(r, kHolderUse), std::size_t{1});
}

// REJECT: the `const` spelling -- §28.2 names both ("an alias (`var b := a;`,
// `const b := a;`)").
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           const_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    const b := c;\n"
        "    c.Dispose();\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `b`"));
}

// REJECT: the alias taken through `!`, the kill spelled `!.Dispose()`.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           bang_alias_and_bang_dispose_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c: Customer? := new Customer(\"ann\");\n"
        "    var b := c!;\n"
        "    c!.Dispose();\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the kill spelled `?.Dispose()` and the use spelled `?.` -- "The
// split is between the kills, never between the use forms" (§28.2).
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           null_conditional_kill_and_use_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c: Customer? := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    c?.Dispose();\n"
        "    Console.WriteLine(b?.Name ?? \"none\");\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the kill spelled as the discard -- §28.2: "or a discard of the
// binding (§17.13)".
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           discard_kill_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    _ := c;\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the use is an ARGUMENT position -- §28.2: "passing it".
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           passing_the_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    c.Dispose();\n"
        "    var n := Sink.Take(b);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the use is a RE-BORROW -- §28.2: "re-borrowing it".
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           reborrowing_the_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    c.Dispose();\n"
        "    var d := b;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `b`"));
}

// REJECT: an alias of an alias borrows the same value.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           alias_of_an_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    var d := b;\n"
        "    c.Dispose();\n"
        "    var n := d.Hello();\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `d`"));
    BEZEL_EXPECT_TRUE(hasError(r, "Dispose() of `c`"));
}

// REJECT: the source is an `owned` PARAMETER.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           alias_of_an_owned_parameter_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        kHead +
        "  public static method Body(owned q: Customer): int\n"
        "    var b := q;\n"
        "    q.Dispose();\n"
        "    Console.WriteLine(b.Name);\n"
        "    return 0;\n"
        "  end Body;\n"
        "  public static method Main(): int\n"
        "    return Body(new Customer(\"q\"));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the kill on BOTH arms dominates the use -- "on every path".
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_on_both_branches_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    if k = 1 then\n"
        "      c.Dispose();\n"
        "    else\n"
        "      c.Dispose();\n"
        "    end;\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the kill BEFORE a loop dominates the use in its body.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_before_a_loop_use_in_its_body_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    c.Dispose();\n"
        "    var i := 0;\n"
        "    while i < 2 do\n"
        "      Console.WriteLine(b.Name);\n"
        "      i := i + 1;\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: "A reassignment of the variable between the kill and the use does
// not lift the error -- the holder still borrows the disposed value" (§28.2).
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           reassignment_between_kill_and_use_does_not_lift_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    c.Dispose();\n"
        "    c := new Customer(\"second\");\n"
        "    Console.WriteLine(b.Name);\n"
        "    c.Dispose();\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `b`"));
}

// ─────────────────────────────── ACCEPT ────────────────────────────────────

// ACCEPT: the same alias with NO disposal.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           alias_without_disposal_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: "a disposal after the borrow's last use changes no verdict" (§28.2).
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           disposal_after_the_last_use_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    Console.WriteLine(b.Name);\n"
        "    c.Dispose();\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: the kill on ONE arm only -- §28.2: "A holder whose source was
// disposed on only some paths to the use ... is not rejected".
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_on_one_branch_only_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    if k = 0 then\n"
        "      c.Dispose();\n"
        "    end;\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: the kill on one arm, the use on the OTHER -- the over-rejection the
// brief names.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_on_one_arm_use_on_the_other_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    if k = 0 then\n"
        "      c.Dispose();\n"
        "    else\n"
        "      Console.WriteLine(b.Name);\n"
        "    end;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: the kill on a path that RETURNS does not reach the use.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_on_a_returning_path_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    if k = 0 then\n"
        "      c.Dispose();\n"
        "      return 1;\n"
        "    end;\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: "a kill at the end of a loop body reaching the next iteration's use"
// (§28.2) -- the use at the top of the body is reached by the entry path too,
// on which nothing is disposed.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_at_the_end_of_a_loop_body_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    var i := 0;\n"
        "    while i < 1 do\n"
        "      Console.WriteLine(b.Name);\n"
        "      c.Dispose();\n"
        "      c := new Customer(\"next\");\n"
        "      i := i + 1;\n"
        "    end;\n"));
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: "a holder re-bound to the variable's new value (`b := a;` after
// `a := new ...;`) takes a fresh borrow" (§28.2).
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           holder_rebound_to_the_new_value_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    c.Dispose();\n"
        "    c := new Customer(\"second\");\n"
        "    b := c;\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: an alias taken from the variable's NEW value -- "taken while the
// variable held that value" (§28.2): the disposed value is not the one the
// alias borrows.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           alias_taken_after_the_reassignment_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    c.Dispose();\n"
        "    c := new Customer(\"second\");\n"
        "    var b := c;\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: a DIFFERENT owner is disposed.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           disposal_of_another_owner_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var j := new Customer(\"j\");\n"
        "    var b := c;\n"
        "    j.Dispose();\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// CONTROL: the DIRECT shape keeps its own diagnostic and its own severity --
// this fix is the holder sentence only (the same-variable promotion is the
// rest of #7435).
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           direct_shape_severity_unchanged_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    c.Dispose();\n"
        "    var n := c.Hello();\n"));
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasWarning(r, kDirectWarning));
    BEZEL_EXPECT_FALSE(hasError(r, kDirectWarning));
}

// ──────────────────────────── try / catch / finally ─────────────────────────

// ACCEPT (over-rejection guard): a kill INSIDE a `try` body does not dominate
// a use in its `catch` arm -- the body may throw before it reaches the
// `Dispose()`, and that path arrives at the handler with `c` alive. Runs clean
// on the base compiler and must stay accepted.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_inside_a_try_use_in_its_catch_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    try\n"
        "      Console.WriteLine(b.Name);\n"
        "      c.Dispose();\n"
        "    catch e: Exception =>\n"
        "      Console.WriteLine(b.Name);\n"
        "    end;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// REJECT: a kill BEFORE the `try` dominates the use in its `catch` arm.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_before_a_try_use_in_its_catch_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    c.Dispose();\n"
        "    try\n"
        "      throw new Exception(\"x\");\n"
        "    catch e: Exception =>\n"
        "      Console.WriteLine(b.Name);\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: a kill inside a `try` body with a `finally` dominates a use AFTER
// the whole statement -- the only path past the `try` ran the kill.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_inside_a_try_use_after_the_try_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    try\n"
        "      c.Dispose();\n"
        "    finally\n"
        "      Console.WriteLine(\"f\");\n"
        "    end try;\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// ACCEPT (over-rejection guard): the same with a multi-block `try` body -- the
// `throw` before the kill is the path that runs, and the handler reads a live
// value. Runs clean on the base compiler.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_in_a_multi_block_try_body_use_in_its_catch_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    try\n"
        "      if k = 1 then throw new Exception(\"boom\"); end;\n"
        "      c.Dispose();\n"
        "      if k = 0 then Console.WriteLine(\"x\"); end;\n"
        "    catch e: Exception =>\n"
        "      Console.WriteLine(b.Name);\n"
        "    end;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT (over-rejection guard): a kill inside a `try` body that has a `catch`
// does not dominate a use AFTER the statement -- the throwing path skips it.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_in_a_try_body_with_a_catch_use_after_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    try\n"
        "      if k = 1 then throw new Exception(\"boom\"); end;\n"
        "      c.Dispose();\n"
        "    catch e: Exception =>\n"
        "      Console.WriteLine(\"caught\");\n"
        "    end;\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT (over-rejection guard): a `finally` runs on the exceptional path too,
// so a kill in its `try` body -- even one followed by `return` -- does not
// dominate a use in the `finally`. Runs clean on the base compiler (the throw
// path is the one taken).
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_then_return_in_a_try_use_in_its_finally_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    try\n"
        "      try\n"
        "        if k = 1 then throw new Exception(\"boom\"); end;\n"
        "        c.Dispose();\n"
        "        return 0;\n"
        "      finally\n"
        "        Console.WriteLine(b.Name);\n"
        "      end try;\n"
        "    catch e2: Exception =>\n"
        "      Console.WriteLine(\"o\");\n"
        "    end;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT (over-rejection guard): the only arm does not catch what the body
// throws, so its kill never runs and the `finally` reads a live value. Runs
// clean on the base compiler.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_in_a_catch_arm_use_in_the_finally_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    try\n"
        "      try\n"
        "        throw new InvalidOperationException(\"x\");\n"
        "      catch e: ArgumentException =>\n"
        "        c.Dispose();\n"
        "      finally\n"
        "        Console.WriteLine(b.Name);\n"
        "      end try;\n"
        "    catch e2: Exception =>\n"
        "      Console.WriteLine(\"o\");\n"
        "    end;\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// REJECT: inside an outer `try`, a kill, an inner try/catch, then the use --
// the inner statement does not make the outer body's kill uncertain.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_before_an_inner_try_inside_an_outer_try_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    try\n"
        "      c.Dispose();\n"
        "      try\n"
        "        Console.WriteLine(\"t\");\n"
        "      catch e: Exception =>\n"
        "        Console.WriteLine(\"i\");\n"
        "      end;\n"
        "      Console.WriteLine(b.Name);\n"
        "    catch e2: Exception =>\n"
        "      Console.WriteLine(\"o\");\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: a kill and the use inside ONE `finally` body.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_and_use_inside_one_finally_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    try\n"
        "      Console.WriteLine(\"t\");\n"
        "    finally\n"
        "      c.Dispose();\n"
        "      Console.WriteLine(b.Name);\n"
        "    end try;\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: a kill inside a `finally` dominates a use after the statement.
BEZEL_TEST(ownership_alias_use_after_dominating_dispose_7435,
           kill_in_a_finally_use_after_the_try_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var c := new Customer(\"ann\");\n"
        "    var b := c;\n"
        "    try\n"
        "      Console.WriteLine(\"t\");\n"
        "    finally\n"
        "      c.Dispose();\n"
        "    end try;\n"
        "    Console.WriteLine(b.Name);\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}
