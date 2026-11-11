// Regression tests for #7441 (the bound-method-reference holder) -- a use of
// a local that holds a BOUND METHOD REFERENCE after a DOMINATING explicit
// disposal of the reference's receiver was accepted.
//
//   var d := new D(7);
//   var f := d.Ping;      // f captures the instance d holds, by reference
//   d.Dispose();          // the disposal DOMINATES the later use
//   return f();           // a USE OF THE HOLDER -- compile error
//
// Language spec §28.2 (v0.357) "Use after a dominating explicit disposal":
//   "The same error reaches a use through a local borrow of the variable's
//    value: where a local holds a borrow of that value -- an alias (`var b :=
//    a;`, `const b := a;`), a `using var` pin (§28.10), a value that holds
//    the borrow (a span over it, a borrow-bearing record or choice, §9.7), a
//    closure that captures such a holder, or an iterator, enumerator or
//    lazy-query frame that carries it ... -- taken while the variable held
//    that value, and every value in the holder's source set (K2.2) has been
//    so disposed on every path to the use, a use of the holder -- a read or
//    member access through it, passing it, re-borrowing it, invoking the
//    closure, enumerating or disposing the frame -- is the same compile
//    error; the diagnostic names the use, the holder, the disposed source and
//    the disposing site. ... A reassignment of the variable between the kill
//    and the use does not lift the error -- the holder still borrows the
//    disposed value ... -- while a holder re-bound to the variable's new
//    value (`b := a;` after `a := new ...;`) takes a fresh borrow. ... A
//    holder whose source was disposed on only some paths to the use ... is
//    not rejected".
//
// Language spec §27.6 (v0.357): "instance methods (the instance is captured
// at formation -- by reference for class- or interface-typed receivers, by
// copy for value-shaped receivers ...)".
//
// Language spec §28.3.8 (v0.357), provenance clause 2: "A **method
// reference** contributes its bound receiver's source, as a capture".
//
// THE READING THIS FILE PINS. §28.2's parenthesis after "a value that holds
// the borrow" names a span and a borrow-bearing record or choice; it does not
// name a bound method reference. §27.6 makes one exactly such a value -- the
// INSTANCE is captured, at the formation, by reference -- and §28.3.8 gives
// it the receiver's source. It is not "a closure that captures the variable
// itself", which "reads the variable when it is invoked": a store into the
// variable after the formation does not re-point the reference (measured on
// the base compiler: the reassignment row below invokes `Ping` on the first,
// released instance -- ASan heap-use-after-free). So the holder sentence
// applies, reassignment clause included.
//
// The rows are COMPILE-TIME (`parseAndCheckOwnership`): the defect is a
// compile-time over-acceptance. Every REJECT row compiled on the base
// compiler (see the commit body for the lane's run-time matrix).

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The diagnostic's stable fragment (§28.2), shared with the alias, record and
// pin holders (#7435, #7441, #7893).
constexpr const char* kHolderUse =
    "after a dominating explicit Dispose() of `d`";
constexpr const char* kUseOfF = "use of `f` after a dominating explicit";
// What this holder kind is called.
constexpr const char* kMethodRef =
    "is a bound method reference that captured that value as its receiver";

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

std::size_t errorCount(const OwnershipResult& r) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++n;
    return n;
}

// The §28.2 holder error, at a use of `f`, naming `d` and the kill site.
bool rejectedAsHolderUse(const OwnershipResult& r) {
    return hasError(r, kUseOfF) && hasError(r, kHolderUse) &&
           hasError(r, kMethodRef) &&
           hasNote(r, "explicit Dispose() of `d` was here");
}

const std::string kHead =
    "module M;\n"
    "public class D\n"
    "  private _n: int;\n"
    "  public constructor(n: int)\n"
    "    _n := n;\n"
    "  end constructor;\n"
    "  public method Ping(): int => _n;\n"
    "  public method Bump(k: int): int => _n + k;\n"
    "  public static method Stat(): int => 5;\n"
    "end D;\n"
    "extension D\n"
    "  public method Twice(): int => this.Ping() * 2;\n"
    "end;\n"
    "public class C\n"
    "  public static method Call(g: () -> int): int => g();\n";

const std::string kTail =
    "  public static method Main(): int\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

std::string prog(const std::string& members) {
    return kHead + members + kTail;
}

}  // namespace

// ─────────────────────────────── REJECT ────────────────────────────────────

// The base shape: form, kill, invoke.
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           invoke_after_dominating_dispose_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(): int\n"
        "    var d := new D(7);\n"
        "    var f := d.Ping;\n"
        "    d.Dispose();\n"
        "    return f();\n"
        "  end Run;\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectedAsHolderUse(r));
    BEZEL_EXPECT_EQ(errorCount(r), static_cast<std::size_t>(1));
}

// §28.2: "A reassignment of the variable between the kill and the use does
// not lift the error -- the holder still borrows the disposed value". The
// reference captured the INSTANCE (§27.6), so it does not see the store.
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           reassignment_between_kill_and_invoke_does_not_lift_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(): int\n"
        "    var d := new D(7);\n"
        "    var f := d.Ping;\n"
        "    d.Dispose();\n"
        "    d := new D(9);\n"
        "    return f();\n"
        "  end Run;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsHolderUse(r));
}

// The kill spelled as a DISCARD (§28.2: "or a discard of the binding").
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           invoke_after_discard_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(): int\n"
        "    var d := new D(7);\n"
        "    var f := d.Ping;\n"
        "    _ := d;\n"
        "    return f();\n"
        "  end Run;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsHolderUse(r));
}

// A nullable receiver read through `!`, killed by `d?.Dispose()`.
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           bang_receiver_and_null_conditional_kill_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(): int\n"
        "    var d: D? := new D(7);\n"
        "    var f := d!.Ping;\n"
        "    d?.Dispose();\n"
        "    return f();\n"
        "  end Run;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsHolderUse(r));
}

// The kill on BOTH arms of an `if` reaches the use on every path.
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           kill_on_both_arms_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(kill: bool): int\n"
        "    var d := new D(7);\n"
        "    var f := d.Ping;\n"
        "    if kill then\n"
        "      d.Dispose();\n"
        "    else\n"
        "      d.Dispose();\n"
        "    end;\n"
        "    return f();\n"
        "  end Run;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsHolderUse(r));
}

// "passing it": a `const` holder handed to a function-typed parameter.
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           const_holder_passed_as_argument_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(): int\n"
        "    var d := new D(7);\n"
        "    const f := d.Ping;\n"
        "    d.Dispose();\n"
        "    return C.Call(f);\n"
        "  end Run;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsHolderUse(r));
}

// "re-borrowing it": binding the dead holder to another local.
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           rebinding_the_dead_holder_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(): int\n"
        "    var d := new D(7);\n"
        "    var f := d.Ping;\n"
        "    d.Dispose();\n"
        "    var g := f;\n"
        "    return 0;\n"
        "  end Run;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsHolderUse(r));
}

// The invocation as an expression STATEMENT, over a method with a parameter.
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           statement_invocation_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(): int\n"
        "    var d := new D(7);\n"
        "    var f := d.Bump;\n"
        "    d.Dispose();\n"
        "    _ := f(1);\n"
        "    return 0;\n"
        "  end Run;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsHolderUse(r));
}

// The receiver is an `owned` PARAMETER (an owner variable, as a local is).
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           owned_parameter_receiver_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Take(owned d: D): int\n"
        "    var f := d.Ping;\n"
        "    d.Dispose();\n"
        "    return f();\n"
        "  end Take;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsHolderUse(r));
}

// A RECEIVER-QUALIFIED extension-method reference is a bound reference too
// (§27.6: "the receiver is captured at formation as the instance-method
// sentence above states").
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           extension_method_reference_rejected_spec_27_6) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(): int\n"
        "    var d := new D(7);\n"
        "    var f := d.Twice;\n"
        "    d.Dispose();\n"
        "    return f();\n"
        "  end Run;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsHolderUse(r));
}

// ─────────────────── the two boundary controls (ADMIT) ─────────────────────

// CONTROL 1 -- a CONDITIONAL kill. §28.2: "A holder whose source was disposed
// on only some paths to the use ... is not rejected". A fix that asked "was
// the receiver ever explicitly disposed?" instead of AND-merging the fact at
// the join would reject this.
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           control_conditional_kill_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(kill: bool): int\n"
        "    var d := new D(7);\n"
        "    var f := d.Ping;\n"
        "    if kill then\n"
        "      d.Dispose();\n"
        "    end;\n"
        "    return f();\n"
        "  end Run;\n"));
    BEZEL_EXPECT_FALSE(hasError(r, "after a dominating explicit Dispose()"));
}

// CONTROL 2 -- a holder RE-BOUND to the variable's new value. §28.2: "a
// holder re-bound to the variable's new value (`b := a;` after `a := new
// ...;`) takes a fresh borrow". A fix that tracked the holder from its
// declaration alone, without noticing the re-binding, would reject this.
BEZEL_TEST(ownership_method_reference_use_after_dominating_dispose_7441,
           control_rebound_holder_admitted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Run(): int\n"
        "    var d := new D(7);\n"
        "    var f := d.Ping;\n"
        "    d.Dispose();\n"
        "    d := new D(9);\n"
        "    f := d.Ping;\n"
        "    return f();\n"
        "  end Run;\n"));
    BEZEL_EXPECT_FALSE(hasError(r, "after a dominating explicit Dispose()"));
}
