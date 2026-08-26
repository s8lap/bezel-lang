// fix #2725 — sema's `new T([...])` rejection (ExprTypecheck_Exprs.cpp,
// ~line 3470 in the pre-fix tree) reported a single rationale for every
// stdlib builtin-collection head — "the constructor takes an optional
// `int` capacity" — that is FALSE for every type in the branch except the
// capacity-bearing ones. The four types called out by the issue are
// SPECIFICALLY WRONG:
//
//   - LinkedList<T>: stdlib §4.6 declares `constructor(source: IEnumerable<T>)`
//     and no `(capacity: int)` form. The old message told the user a
//     constructor that does not exist, AND denied the existence of the
//     constructor that does.
//   - StringBuilder: stdlib §3.1 declares three constructors — `()`,
//     `(capacity: int)`, AND `(initial: string)`. The old message hid the
//     `(initial: string)` form (so a user who wanted a StringBuilder from a
//     string was steered wrong).
//   - Shared<T>: stdlib §2.16 declares `constructor(owned value: T)` — a
//     single-value constructor, NOT a capacity.
//   - Weak<T>: stdlib §2.16 declares NO constructors at all — `new Weak<T>(...)`
//     was already rejected upstream by constructor-lookup, but the rationale
//     would have been false if the branch ever fired.
//
// The fix is per-type rationale: each stdlib type gets the rationale that
// matches its ACTUAL constructor shape per its stdlib declaration.
//
// REJECTION ITSELF IS NOT NEW — the rejection at the call site predates
// #2725 (#599). What #2725 changes is the WHAT-IS-TAKEN half of the message
// (it used to be wrong for ~9 of the 14 types in the branch). The rejection
// ITSELF stays as it was: a collection literal in a `new` argument position
// is rejected because §15.4 requires a collection-typed target context for
// `[..]` and `new`'s argument position supplies no target type.
//
// SPEC CITATIONS:
//   - §15.4 "Collection literals" (v0.282): "A collection literal has no
//     standalone type. It is well-typed only in a context that provides the
//     target type, either via type annotation or via the parameter type of a
//     method call" — the position-supplies-no-target property that drives
//     the rejection.
//   - §22.7 "Constructors" (v0.282): constructor signature declarations for
//     the stdlib heads named in the diagnostic (mirrored in
//     docs/Bezel_Standard_Library.md §3.1, §4.4–§4.11, §2.16).
//   - §28.13 "Shared/Weak" (v0.282) for the Shared/Weak constructor shape.
//
// (issue #2725, spec v0.282)
//
// fail-without / pass-with: on `d34c8b4` every BEZEL_EXPECT_FALSE(r.ran) row
// below fails — the rejection fires (so `r.ran` is false, which seems like
// "pass") but the message carries the OLD false "optional `int` capacity"
// claim for LinkedList/StringBuilder/Shared/Weak (and the Boolean helper
// `hasOldFalseRationale` returns true, so the test reports a regression).
// With the fix, each diagnostic carries the CORRECT constructor-shape
// claim for its type.
//
// What this test asserts:
//   1. The rejection still fires (no widening — no accept-rows drift to
//     reject, no reject-rows drift to accept).
//   2. The diagnostic carries the type-correct rationale — for the four
//     issue-named types, the message MUST NOT claim `int` capacity as the
//     only constructor shape.
//   3. The diagnostic cites §15.4 (the spec authority) and #2725 (the
//     per-type rationale is the #2725 fix).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Module prelude + C class hosting `static Main`.
const char* kPrelude =
    "module M; ";

std::string prog(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// True iff SOME diagnostic carries the rejection signature: a `new T<...>`
// argument position receiving a collection literal, citing §15.4 and #2725.
// Anchored on the SPEC citation + the issue number so a future diagnostic
// touching §15.4 (e.g. the borrowing-family fresh-producer rejection, the
// target-not-on-list rejection, the dict-form mismatch, the inline-array
// arity error) cannot satisfy it.
bool rejectedAtCtorArg(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`[..]`") != std::string::npos &&
            d.message.find("`new ") != std::string::npos &&
            d.message.find("not a collection literal") != std::string::npos &&
            d.message.find("\xC2\xA7""15.4") != std::string::npos &&
            d.message.find("#2725") != std::string::npos)
            return true;
    return false;
}

// Debug helper: write the diagnostics to stderr so a test failure tells the
// reader which fragment of the rejection the AOT path emitted (the bezelc
// CLI diagnostic and the runAot diagnostic can differ in detail; this
// pins which one the test actually saw).
void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

// True iff the OLD false rationale fragment appears in any diagnostic.
// This is the exact pre-fix wording: "the constructor takes an optional
// `int` capacity, not a collection literal". Post-fix the rationale is
// per-type; even for the capacity-bearing types the message is wrapped in
// additional context (e.g. the sized-fill pair for Array) so the bare
// "takes an optional `int` capacity" fragment does NOT appear standalone.
bool hasOldFalseRationale(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("takes an optional `int` capacity, not a collection literal")
            != std::string::npos)
            return true;
    return false;
}

// True iff SOME diagnostic carries the per-type rationale that #2725
// installs. Each test row names the type and the per-type rationale
// fragment.
bool hasRationaleFor(const JitResult& r, const std::string& fragment) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(fragment) != std::string::npos)
            return true;
    return false;
}

// Assert the per-type rationale pin: program rejected, rejection cites
// §15.4 + #2725, rationale carries the type-correct fragment.
void expectPinRationale(const std::string& body, const std::string& perTypeFragment) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    if (!rejectedAtCtorArg(r)) {
        std::fprintf(stderr, "rejectedAtCtorArg FAILED:\n");
        dumpDiagnostics(r);
    }
    if (!hasRationaleFor(r, perTypeFragment)) {
        std::fprintf(stderr, "hasRationaleFor FAILED for fragment '%s':\n",
                     perTypeFragment.c_str());
        dumpDiagnostics(r);
    }
    BEZEL_EXPECT_TRUE(rejectedAtCtorArg(r));
    BEZEL_EXPECT_TRUE(hasRationaleFor(r, perTypeFragment));
}

// =====================================================================
// REJECT — the four issue-named types, each pinned to its per-type rationale.
// =====================================================================

// `LinkedList<T>` declares `constructor(source: IEnumerable<T>)` and no
// `(capacity: int)` form. The pre-fix message denied the existence of the
// source constructor AND told the user a constructor that does not exist.
// Post-fix the message names the IEnumerable form. // per §15.4, §4.6 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           linked_list_rejection_names_ienumerable) {
    expectPinRationale(
        "    var xs := new LinkedList<int>([1, 2, 3]); return 0; ",
        "IEnumerable<T>");
}

// `StringBuilder` declares `()`, `(capacity: int)`, AND `(initial: string)`.
// The pre-fix message hid the `(initial: string)` form; post-fix names both.
// // per §15.4, §3.1 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           string_builder_rejection_names_initial_string) {
    expectPinRationale(
        "    var sb := new StringBuilder([1, 2, 3]); return 0; ",
        "string` initial value");
}

// `Shared<T>` declares `constructor(owned value: T)` — a single-value
// constructor, not a capacity. The pre-fix message said `int` capacity,
// which was false AND told the user to use a typed target — but a Shared is
// typed on a CLASS value, not a collection. Post-fix names the owned value
// shape. // per §15.4, §2.16, §28.13 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           shared_rejection_names_owned_value) {
    expectPinRationale(
        "    var s := new Shared<S>([1, 2, 3]); return 0; ",
        "owned` value of `T");
}

// `Weak<T>` declares NO constructors at all. The pre-fix message said
// `int` capacity — false. Post-fix names the no-constructor reality.
// // per §15.4, §2.16 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           weak_rejection_names_no_constructor) {
    expectPinRationale(
        "    var w := new Weak<S>([1, 2, 3]); return 0; ",
        "declares no constructors");
}

// =====================================================================
// REJECT — adjacent coverage: OwningLinkedList, Stack, OwningStack, Queue,
// OwningQueue all declare `()` only and were wrong under the old rationale.
// =====================================================================

// `OwningLinkedList<T>` declares only `()` — no parameter form at all.
// Post-fix names this. // per §15.4, §4.7 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           owning_linked_list_rejection_names_no_args) {
    expectPinRationale(
        "    var xs := new OwningLinkedList<int>([1, 2, 3]); return 0; ",
        "takes no arguments");
}

// `Stack<T>` declares only `()` — no `(capacity: int)` form, despite the
// pre-fix message's claim. Post-fix names this. // per §15.4, §4.11 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           stack_rejection_names_no_args) {
    expectPinRationale(
        "    var xs := new Stack<int>([1, 2, 3]); return 0; ",
        "takes no arguments");
}

// `OwningStack<T>` — same as Stack. // per §15.4, §4.11 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           owning_stack_rejection_names_no_args) {
    expectPinRationale(
        "    var xs := new OwningStack<int>([1, 2, 3]); return 0; ",
        "takes no arguments");
}

// `Queue<T>` — same as Stack. // per §15.4, §4.11 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           queue_rejection_names_no_args) {
    expectPinRationale(
        "    var xs := new Queue<int>([1, 2, 3]); return 0; ",
        "takes no arguments");
}

// `OwningQueue<T>` — same as Stack. // per §15.4, §4.11 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           owning_queue_rejection_names_no_args) {
    expectPinRationale(
        "    var xs := new OwningQueue<int>([1, 2, 3]); return 0; ",
        "takes no arguments");
}

// =====================================================================
// REJECT — capacity-bearing types STILL mention capacity, but the per-type
// shape carries the additional overloads too. These pin the OPPOSITE
// direction: a fix that deleted the capacity claim entirely would now
// understate for Array/OwningArray/Set/OwningSet/Dictionary/OwningDictionary.
// =====================================================================

// `Array<T>` declares `(capacity: int)` AND `(size: int, fill: T)` — the
// sized-fill pair is the form the user might plausibly want for a literal.
// Post-fix names both. // per §15.4, §4.4 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           array_rejection_names_capacity_and_sized_fill) {
    expectPinRationale(
        "    var xs := new Array<int>([1, 2, 3]); return 0; ",
        "sized-fill pair");
}

// `Set<T>` declares `(capacity: int)` AND `(comparer: IEqualityComparer<K>)`
// AND `(capacity, comparer)`. Post-fix names the capacity + comparer
// variants. // per §15.4, §4.10 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           set_rejection_names_capacity_and_comparer) {
    expectPinRationale(
        "    var xs := new Set<int>([1, 2, 3]); return 0; ",
        "IEqualityComparer");
}

// =====================================================================
// NEGATIVE — the OLD false rationale MUST NOT appear for any non-capacity
// type. Pre-fix every BEZEL_EXPECT_FALSE below fails (the OLD rationale is
// present for ALL of them, including LinkedList which is the issue's
// cardinal repro). Post-fix the rationale is per-type and the OLD false
// fragment never appears.
// =====================================================================

// The issue's cardinal repro: LinkedList must not be told about an `int`
// capacity constructor that does not exist. // per §15.4, §4.6 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           linked_list_diagnostic_omits_old_false_capacity_claim) {
    JitResult r = runAot(prog(
        "    var xs := new LinkedList<int>([1, 2, 3]); return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(hasOldFalseRationale(r));
}

// Stack/OwningStack/Queue/OwningQueue/OwningLinkedList: same defect, same
// claim, all false. // per §15.4, §4.7, §4.11 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           stack_omits_old_false_capacity_claim) {
    JitResult r = runAot(prog(
        "    var xs := new Stack<int>([1, 2, 3]); return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(hasOldFalseRationale(r));
}

BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           queue_omits_old_false_capacity_claim) {
    JitResult r = runAot(prog(
        "    var xs := new Queue<int>([1, 2, 3]); return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(hasOldFalseRationale(r));
}

BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           owning_linked_list_omits_old_false_capacity_claim) {
    JitResult r = runAot(prog(
        "    var xs := new OwningLinkedList<int>([1, 2, 3]); return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(hasOldFalseRationale(r));
}

// Shared/Weak: same defect, also told about an `int` capacity constructor.
// // per §15.4, §2.16, §28.13 (#2725)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           shared_omits_old_false_capacity_claim) {
    JitResult r = runAot(prog(
        "    var s := new Shared<S>([1, 2, 3]); return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(hasOldFalseRationale(r));
}

BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           weak_omits_old_false_capacity_claim) {
    JitResult r = runAot(prog(
        "    var w := new Weak<S>([1, 2, 3]); return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(hasOldFalseRationale(r));
}

// =====================================================================
// ACCEPT-CONTROL — the rejection is LITERAL-only and the typed-target
// declaration remedy (the OLD message's named remedy, unchanged by #2725)
// still works. A fix that widened to ingress would reject this row; a fix
// that broke the remedy text would silently regress users. // per §15.4
// (remedy paragraph, v0.282)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           typed_target_declaration_remedy_still_admitted) {
    // From §15.4, verbatim: "using var b: Array<int> := [1, 2, 3];" — the
    // very remedy the diagnostic has always named. This row must keep
    // passing both before and after the fix; it's the control.
    std::string body =
        "    using var xs: Array<int> := [1, 2, 3]; "
        "    return xs.Count; ";
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 3);
}

// LinkedList variant of the same control — also admitted both pre- and
// post-fix. // per §15.4, §4.6 (remedy paragraph, v0.282)
BEZEL_TEST(sema_collection_literal_ctor_arg_per_type_rationale_2725,
           linked_list_typed_target_remedy_admitted) {
    std::string body =
        "    using var xs: LinkedList<int> := [1, 2, 3]; "
        "    return xs.Count; ";
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 3);
}

}  // namespace