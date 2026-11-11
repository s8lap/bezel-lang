// Regression tests for #7441 -- a use of a BORROW-BEARING RECORD local after
// a DOMINATING explicit disposal of the variable whose value it borrows was
// accepted, and the program then faulted on every run
// (`DisposedAccessException` in checked disposal mode, a heap-use-after-free
// under the generated-code ASan leg unchecked).
//
//   var i := new Item("i");
//   var p := new Pair(i);             // p HOLDS a borrow of i's value (§9.7)
//   i.Dispose();                      // the disposal DOMINATES the later use
//   Console.WriteLine(p.Item!.Name);  // a USE OF THE HOLDER -- compile error
//
// Language spec §28.2 (v0.357) "Use after a dominating explicit disposal":
//   "The same error reaches a use through a local borrow of the variable's
//    value: where a local holds a borrow of that value -- an alias ..., a
//    `using var` pin (§28.10), a value that holds the borrow (a span over it,
//    a borrow-bearing record or choice, §9.7), ... -- taken while the variable
//    held that value, and every value in the holder's source set (K2.2) has
//    been so disposed on every path to the use, a use of the holder -- a read
//    or member access through it, passing it, re-borrowing it, ... -- is the
//    same compile error; the diagnostic names the use, the holder, the
//    disposed source and the disposing site."
// and §9.7 (v0.357) *Member stores*: a store into a member of a
// borrow-bearing local "adds the sources of the borrows `e` carries to that
// set, never removing one" -- so a record local whose member is stored into
// anywhere in the method has a source set its construction does not fix.
//
// Root cause: the same gap as #7435 -- nothing asked, at a use of the holder,
// whether the value it borrows had been disposed -- for the record holder: a
// record built over an owner (`new Pair(i)`) carries `i`'s borrow in a
// component, and its local was never connected to `i`'s disposal.
//
// The fix binds, at `var p := new R(args);` / `const p := new R(args);` of a
// positional borrow-bearing record, the holder's source set from the
// arguments (`LocalOwnership::heldBorrows`, #7435's holder-side fact), and
// rejects a use of the holder once EVERY source has been explicitly disposed
// on every path to it.
//
// The rows are COMPILE-TIME (`parseAndCheckOwnership`). Every REJECT row was
// measured faulting on the base compiler -- except the pure "passing it" row,
// whose callee never dereferences, which §28.2 rejects by its use-form list
// -- and every ACCEPT row running clean (the lane's matrix; see the commit
// body). The first REJECT row is the issue's own repro, byte for byte.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The diagnostic's stable fragment (§28.2), shared with the alias holder
// (#7435) and the pin holder (#7893).
constexpr const char* kHolderUse =
    "after a dominating explicit Dispose() of";

// The record wording of the same diagnostic.
constexpr const char* kRecordHolder = "is a borrow-bearing record";

bool hasError(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
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

const std::string kHead =
    "module Probe;\n"
    "public class Item\n"
    "  private _name: string;\n"
    "  public constructor(name: string)\n"
    "    _name := name;\n"
    "  end constructor;\n"
    "  public property Name: string => _name;\n"
    "end Item;\n"
    "public record Pair(Item: Item?);\n"
    "public record Two(A: Item?, B: Item?);\n"
    "public record Tagged(Item: Item?, N: int);\n"
    "public static class Sink\n"
    "  public static method TakePair(p: Pair): int => 1;\n"
    "end Sink;\n"
    "public class Program\n"
    "  public static method Body(k: int): int\n";

const std::string kTail =
    "    return 0;\n"
    "  end Body;\n"
    "  public static method Main(): int\n"
    "    return Body(1);\n"
    "  end Main;\n"
    "end Program;\n";

std::string prog(const std::string& body) {
    return kHead + body + kTail;
}

}  // namespace

// ─────────────────────────────── REJECT ────────────────────────────────────

// REJECT: the issue's own repro (probe p05), byte for byte.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           issue_repro_record_holder_member_access_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        "module Probe;\n"
        "public class Item\n"
        "  private _name: string;\n"
        "  public constructor(name: string)\n"
        "    _name := name;\n"
        "  end constructor;\n"
        "  public property Name: string => _name;\n"
        "end Item;\n"
        "public record Pair(Item: Item?);\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var i := new Item(\"i\");\n"
        "    var p := new Pair(i);\n"
        "    i.Dispose();\n"
        "    Console.WriteLine(p.Item!.Name);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasError(r, kRecordHolder));
    // "the diagnostic names the use, the holder, the disposed source and the
    // disposing site" (§28.2).
    BEZEL_EXPECT_TRUE(hasError(r, "use of `p`"));
    BEZEL_EXPECT_TRUE(hasError(r, "Dispose() of `i`"));
    BEZEL_EXPECT_TRUE(hasNote(r, "explicit Dispose() of `i` was here"));
}

// REJECT: the `const` holder.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           const_record_holder_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    const p := new Pair(i);\n"
        "    i.Dispose();\n"
        "    Console.WriteLine(p.Item!.Name);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `p`"));
}

// REJECT: the use is an ARGUMENT position -- §28.2: "passing it".
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           passing_the_record_holder_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var p := new Pair(i);\n"
        "    i.Dispose();\n"
        "    var n := Sink.TakePair(p);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: a record with a VALUE component beside the borrow.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           record_with_a_value_component_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var p := new Tagged(i, 3);\n"
        "    i.Dispose();\n"
        "    Console.WriteLine(p.Item!.Name);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: TWO sources, BOTH disposed -- "every value in the holder's source
// set (K2.2) has been so disposed" (§28.2); the diagnostic names both.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           two_sources_both_disposed_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var j := new Item(\"j\");\n"
        "    var p := new Two(i, j);\n"
        "    i.Dispose();\n"
        "    j.Dispose();\n"
        "    Console.WriteLine(p.B!.Name);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasError(r, "`i`"));
    BEZEL_EXPECT_TRUE(hasError(r, "`j`"));
    BEZEL_EXPECT_TRUE(hasNote(r, "explicit Dispose() of `i` was here"));
    BEZEL_EXPECT_TRUE(hasNote(r, "explicit Dispose() of `j` was here"));
}

// REJECT: the record built over an ALIAS of the owner borrows the same value.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           record_built_over_an_alias_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var b := i;\n"
        "    var p := new Pair(b);\n"
        "    i.Dispose();\n"
        "    Console.WriteLine(p.Item!.Name);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasError(r, "Dispose() of `i`"));
}

// REJECT: the kill on BOTH arms dominates the use.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           kill_on_both_branches_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var p := new Pair(i);\n"
        "    if k = 1 then\n"
        "      i.Dispose();\n"
        "    else\n"
        "      i.Dispose();\n"
        "    end;\n"
        "    Console.WriteLine(p.Item!.Name);\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: "A reassignment of the variable between the kill and the use does
// not lift the error -- the holder still borrows the disposed value" (§28.2).
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           reassignment_between_kill_and_use_does_not_lift_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var p := new Pair(i);\n"
        "    i.Dispose();\n"
        "    i := new Item(\"second\");\n"
        "    Console.WriteLine(p.Item!.Name);\n"
        "    i.Dispose();\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// ─────────────────────────────── ACCEPT ────────────────────────────────────

// ACCEPT: the same record holder with NO disposal.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           record_holder_without_disposal_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var p := new Pair(i);\n"
        "    Console.WriteLine(p.Item!.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: "a disposal after the borrow's last use changes no verdict" (§28.2).
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           disposal_after_the_last_use_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var p := new Pair(i);\n"
        "    Console.WriteLine(p.Item!.Name);\n"
        "    i.Dispose();\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: TWO sources, only ONE disposed -- not "every value in the holder's
// source set" (§28.2); the use reads the live one.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           two_sources_one_disposed_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var j := new Item(\"j\");\n"
        "    var p := new Two(i, j);\n"
        "    i.Dispose();\n"
        "    Console.WriteLine(p.B!.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: the kill on ONE arm only -- "disposed on only some paths to the
// use ... is not rejected" (§28.2).
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           kill_on_one_branch_only_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var p := new Pair(i);\n"
        "    if k = 0 then\n"
        "      i.Dispose();\n"
        "    end;\n"
        "    Console.WriteLine(p.Item!.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: a record built over `null` carries no borrow of the disposed owner.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           null_component_record_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var p := new Pair(null);\n"
        "    i.Dispose();\n"
        "    Console.WriteLine(p.Item?.Name ?? \"none\");\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: a holder RE-BOUND to a live source -- "a holder re-bound to the
// variable's new value ... takes a fresh borrow" (§28.2).
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           record_holder_rebound_to_a_live_source_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var j := new Item(\"j\");\n"
        "    var p := new Pair(i);\n"
        "    i.Dispose();\n"
        "    p := new Pair(j);\n"
        "    Console.WriteLine(p.Item!.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ───────────────────────── a component of the holder ───────────────────────

// REJECT: an alias of a record COMPONENT (`var x := p.Item;`) borrows what the
// record borrows; the use through it after the dominating disposal is the same
// error ("a read or member access through it", §28.2).
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           alias_of_a_record_component_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var p := new Pair(i);\n"
        "    var x := p.Item;\n"
        "    i.Dispose();\n"
        "    Console.WriteLine(x!.Name);\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `x`"));
}

// ACCEPT: a copy of a VALUE component borrows nothing.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           copy_of_a_value_component_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var t := new Tagged(i, 3);\n"
        "    var n := t.N;\n"
        "    i.Dispose();\n"
        "    Console.WriteLine(n);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: an alias of one component of a two-source record whose OTHER source
// is disposed -- not every value in the source set (K2.2) is disposed.
BEZEL_TEST(ownership_record_holder_use_after_dominating_dispose_7441,
           component_alias_other_source_disposed_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var i := new Item(\"i\");\n"
        "    var j := new Item(\"j\");\n"
        "    var p := new Two(i, j);\n"
        "    var x := p.B;\n"
        "    i.Dispose();\n"
        "    Console.WriteLine(x!.Name);\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}
