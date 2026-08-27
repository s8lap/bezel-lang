// Spec-derived regression for issue #3364 — the §15.4 FRESH-STORE WARNING.
//
// WHAT #3364 CLAIMED, AND WHY IT IS REFUTED. #3364 was filed as a #3356
// follow-up asserting that `for var x in h[0] do … end` over a `Set<Item>`
// reached through an expression-bodied indexer needed "a wrap primitive
// distinct from the property path's". Probed on e9bd762, the symptom (a
// heap-use-after-free: the Items are freed inside `bz$M.Holder$ctor` and read
// in `main`) reproduces IDENTICALLY without the indexer, without the property,
// on a plain local, and on `Array<Item>` as well as `Set<Item>` — so it is not
// the wrap, and not `Set`. The store is what is wrong, not the read.
//
// AND THE STORE IS ADMITTED. §28.11 *Borrowing collections* (v0.243):
//
//   "A **fresh unbound owned temporary** of class type passed to
//    `Add`/`Insert`/indexer-set is not a borrow of any named source but a
//    temporary that dies at its K8 boundary (§28.3): the store is admitted
//    under §15.4's recorded literal-only-rejection asymmetry and draws that
//    section's fresh-store warning."
//
// and §15.4 states the obligation this test pins:
//
//   "Because the retention is statically certain to outlive the value, the
//    compiler **must warn** when a store-position argument on a
//    **borrowing-family** collection — `Add`/`Insert` and the indexer-set, key
//    and value positions alike — is a **fresh unbound owned temporary of class
//    type** (§28.3): a `new` expression, a collection literal, an
//    `owned`-returning call result, a `move` result, or a **`Task`-family call
//    result** … The warning names the K8 boundary at which the argument dies
//    and the remedies … The warning does not narrow the admission: the program
//    remains accepted, no `using var` is demanded, and the recorded
//    literal-only-rejection asymmetry above stands unchanged."
//
// So the compiler defect #3364's repro exposes is not a missing rejection and
// not a broken wrap — it is the MISSING MANDATORY WARNING. Measured on
// e9bd762: `_s.Add(new Item(5));` into an `owned Set<Item>` field compiled with
// rc=0 and ZERO diagnostics while the compiler already knew the argument dies
// at the statement's K8 boundary.
//
// §15.4 also names a collection literal in the fresh set. That form is NOT
// pinned here: a literal in a borrowing-collection store position does not
// compile at all on this HEAD (`groups["odds"] := [1, 3, 5];` degrades with
// #3025's "indexer-setter RHS expression failed to lower", and
// `outer.Add([1, 2, 3]);` / `s.Push([1, 2, 3]);` with the #2457-class "no
// dispatch arm emitted any code"), so there is no admitted program to warn on.
// The residual is recorded at the predicate.
//
// // per §15.4 (fresh-store warning, v0.243), §28.11 *Borrowing collections*
//    and *The element slot policy*, §28.3 *Unbound owned temporaries*

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// `Item` is trivial-dispose (no destructor, no owned field — §28.9); `Res`
// carries a destructor, so it is non-trivial-dispose. §15.4 splits the stated
// hazard between them, and both arms are asserted below.
const char* kPreamble =
    "module M; "
    "record Pt(X: int, Y: int); "
    "class Item "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item; "
    "class Res "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor Console.WriteLine(\"dtor\"); end destructor; "
    "end Res; ";

OwnershipResult check(const std::string& body) {
    return parseAndCheckOwnership(std::string(kPreamble) + body);
}

// Counts the §15.4 fresh-store WARNINGs. Keyed on a stable rule identifier —
// the §-reference plus the rule's own name — never on the whole sentence, so a
// wording tweak does not silently break the test (the tests/README convention
// hasErrorMessage documents for the error side).
int freshStoreWarnings(const OwnershipResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("§15.4 fresh-store warning") == std::string::npos)
            continue;
        ++n;
    }
    return n;
}

// Counts EVERY warning that claims a fresh unbound owned temporary, whatever
// its wording. This is the DOUBLE-FIRE GUARD, and it is load-bearing: a sibling
// lane's `fix #3383` (a31d37d, on `cycle-1-integration` when this landed)
// implements a narrower version of this same §15.4 mandate as a second,
// independent loop at the tail of `checkCollectionBorrowStoreEscape`. The two
// hunks do not textually conflict, so applying both would silently emit TWO
// warnings for one store — the exact "two predicates for one classification"
// failure this fix exists to end. Asserting the total (not just this fix's own
// message) makes that collision a red test instead of a quiet duplicate.
int anyFreshTempWarnings(const OwnershipResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("fresh unbound owned temporary") == std::string::npos)
            continue;
        ++n;
    }
    return n;
}

// The text of the first §15.4 fresh-store warning, or "" if none fired.
std::string freshStoreText(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("§15.4 fresh-store warning") != std::string::npos)
            return d.message;
    }
    return std::string();
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── The #3364 repro itself: an `owned Set<Item>` field filled with temporaries ──

BEZEL_TEST(fresh_store_warning_3364, set_field_add_temp_warns_once_per_store) {
    OwnershipResult r = check(
        "class Holder "
        "  private owned _s: Set<Item>; "
        "  public constructor() "
        "    _s := new Set<Item>(); "
        "    _s.Add(new Item(5)); "
        "    _s.Add(new Item(10)); "
        "    _s.Add(new Item(20)); "
        "  end constructor; "
        "end Holder;");
    // §15.4's must-warn is per store-position argument: three stores, three
    // warnings. Pre-fix this was 0.
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 3);
    // …and exactly three in total, from any wording (see anyFreshTempWarnings).
    BEZEL_EXPECT_EQ(anyFreshTempWarnings(r), 3);
    // "The warning does not narrow the admission" — it is a WARNING, and the
    // ownership pass reports no error for the shape.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.4: "The warning names the K8 boundary at which the argument dies and the
// remedies". Both remedies are named: the named holder, and the owning family.
BEZEL_TEST(fresh_store_warning_3364, warning_names_k8_boundary_and_both_remedies) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var a := new Array<Item>(); "
        "  a.Add(new Item(1)); "
        "  return a.Count; "
        "end M; end C;");
    const std::string w = freshStoreText(r);
    BEZEL_EXPECT_FALSE(w.empty());
    BEZEL_EXPECT_TRUE(contains(w, "K8 full-expression boundary"));
    BEZEL_EXPECT_TRUE(contains(w, "named holder"));
    BEZEL_EXPECT_TRUE(contains(w, "OwningArray"));
    BEZEL_EXPECT_TRUE(contains(w, "§28.11"));
}

// §15.4: the hazard "splits by family". A trivial-dispose argument's boundary
// event is the compiler's free (checked-mode `DisposedAccessException`); a
// non-trivial-dispose class's is the value's disposal, leaving the slot
// dangling.
BEZEL_TEST(fresh_store_warning_3364, hazard_text_splits_trivial_vs_non_trivial) {
    OwnershipResult triv = check(
        "class C public static method M(): int "
        "  using var a := new Array<Item>(); "
        "  a.Add(new Item(1)); "
        "  return a.Count; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(contains(freshStoreText(triv), "DisposedAccessException"));

    OwnershipResult nont = check(
        "class C public static method M(): int "
        "  using var a := new Array<Res>(); "
        "  a.Add(new Res(1)); "
        "  return a.Count; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(contains(freshStoreText(nont), "a disposed instance"));
}

// §15.4 names the indexer-set alongside `Add`/`Insert`, so the two store sinks
// answer with the one predicate. `t` is a borrowed parameter, so the `.Add(t)`
// that seeds the slot is itself admitted and unwarned — the single warning
// below is the indexer-set's.
BEZEL_TEST(fresh_store_warning_3364, indexer_set_of_a_fresh_temp_warns) {
    OwnershipResult r = check(
        "class C public static method M(t: Item): int "
        "  using var a := new Array<Item>(); "
        "  a.Add(t); "
        "  a[0] := new Item(2); "
        "  return a.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 1);
    BEZEL_EXPECT_TRUE(contains(freshStoreText(r), "coll[i]"));
}

// §15.4's fresh set includes "an `owned`-returning call result".
BEZEL_TEST(fresh_store_warning_3364, owned_returning_call_result_warns) {
    OwnershipResult r = check(
        "class C "
        "  public static method Make(): owned Res return new Res(7); end Make; "
        "  public static method M(): int "
        "    using var a := new Array<Res>(); "
        "    a.Add(Make()); "
        "    return a.Count; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 1);
}

// The per-collection store spellings are presentations of §28.11's one store
// row, so `Push` is the same rule as `Add`.
BEZEL_TEST(fresh_store_warning_3364, stack_push_of_a_fresh_temp_warns) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var s := new Stack<Item>(); "
        "  s.Push(new Item(3)); "
        "  return s.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 1);
}

// ── Negative controls: the warning must not widen past §15.4's population ──

// The owning family's store positions TAKE ownership (§28.11 owned column) —
// this is one of the two remedies the warning names, so warning on it would
// send the reader in a circle.
BEZEL_TEST(fresh_store_warning_3364, owning_family_store_does_not_warn) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var o := new OwningArray<Item>(); "
        "  o.Add(new Item(1)); "
        "  return o.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 0);
}

// The documented remedy itself: a named holder is not an unbound temporary.
BEZEL_TEST(fresh_store_warning_3364, named_holder_store_does_not_warn) {
    OwnershipResult r = check(
        "class C public static method M(): int "
        "  using var t := new Item(1); "
        "  using var a := new Array<Item>(); "
        "  a.Add(t); "
        "  return a.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 0);
}

// §15.4 scopes the warning to a fresh temporary of CLASS type; §28.11's element
// table puts the other categories in columns that cannot dangle — a value type
// "copies the value in", a runtime-managed reference "stores the reference".
BEZEL_TEST(fresh_store_warning_3364, value_and_runtime_managed_elements_do_not_warn) {
    OwnershipResult ints = check(
        "class C public static method M(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(5); "
        "  return a.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(ints), 0);

    OwnershipResult strs = check(
        "class C public static method M(p: string): int "
        "  using var a := new Array<string>(); "
        "  a.Add(p + \"x\"); "
        "  return a.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(strs), 0);

    OwnershipResult recs = check(
        "class C public static method M(): int "
        "  using var a := new Array<Pt>(); "
        "  a.Add(new Pt(1, 2)); "
        "  return a.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(recs), 0);
}

// `AddRange` is the BULK store: its argument is a COLLECTION whose elements are
// copied out, so the source collection's own death leaves no dangling element
// slot. §15.4 names the element store positions only.
BEZEL_TEST(fresh_store_warning_3364, bulk_add_range_does_not_warn) {
    OwnershipResult r = check(
        "class C public static method M(src: Array<int>): int "
        "  using var a := new Array<int>(); "
        "  a.AddRange(src); "
        "  return a.Count; "
        "end M; end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 0);
}

// The scope that makes the rule implementable at all (§15.4: "rejection cannot
// be scoped by signature — `coll.Add(new X())` and `sink.Consume(new X())` are
// indistinguishable plain-parameter calls"): the warning keys on the RECEIVER
// being a borrowing-family collection, so an ordinary callee taking a fresh
// temporary is untouched.
BEZEL_TEST(fresh_store_warning_3364, plain_callee_taking_a_fresh_temp_does_not_warn) {
    OwnershipResult r = check(
        "class C "
        "  public static method Consume(r: Item): int return r.V; end Consume; "
        "  public static method M(): int return Consume(new Item(9)); end M; "
        "end C;");
    BEZEL_EXPECT_EQ(freshStoreWarnings(r), 0);
}

// ── Admission is unchanged (§15.4: "the program remains accepted") ──

// The #3364 repro end-to-end through the real driver: it still compiles, links,
// and runs. The warning is a signal, not a rejection.
BEZEL_TEST(fresh_store_warning_3364, warned_program_still_compiles_and_runs) {
    auto r = runAot(
        std::string(kPreamble) +
        "class Holder "
        "  private owned _s: Set<Item>; "
        "  public constructor() "
        "    _s := new Set<Item>(); "
        "    _s.Add(new Item(5)); "
        "  end constructor; "
        "  public property Count: int => _s.Count; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  Console.WriteLine(\"count=\" + h.Count.ToString()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("count=1") != std::string::npos);
}
