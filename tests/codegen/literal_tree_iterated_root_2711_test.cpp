// Codegen — issue #2711 (§15.4 collection literals, §21.4 `for` iteration,
// §28.9 runtime-managed / trivial-dispose types, §28.11 borrowing collections;
// spec v0.240): a literal-tree root that is ITERATED installed no interior-node
// disposer, so the entire interior of the tree leaked, once per scope entry.
//
// #2660 made the root's scope-end reclaim carry an interior-node disposer, but
// only for a root satisfying three conditions, the third of which — "no INTERIOR
// node read out of the root may outlive the root's scope" — is decided by a
// purpose-built default-deny walk (InteriorAliasWalker). That walk's `ForStmt`
// arm walked the iterable in a NON-node position, so `for var row in rows` was a
// bare mention of the root and denied outright, taking the whole tree's interior
// reclaim with it.
//
// MEASURED on the pre-fix build (07cc2e0), the issue's own repro over 20 scope
// entries: 4591 bytes in 140 allocations — 7 blocks per tree (2 `bz_llist_t`
// structs, 3 `llist_node_alloc` nodes, 2 string blocks), scaling linearly with
// the number of times the scope is entered, NOT with the iteration count of the
// loops inside it. The root's OWN backing was already reclaimed (it is absent
// from the pre-fix leak report), which is what pins the defect to the interior
// disposer specifically.
//
// The `using var` in the issue title is NOT the discriminator: bisected on the
// pre-fix build, `using var` + indexed read-back and plain `var` + indexed
// read-back are both LSan-clean (exit 60), while `using var` + iteration and
// plain `var` + iteration BOTH leak the identical 4591 bytes / 140 allocations.
// The registration site is reached in every form; it is the ELIGIBILITY
// computation that iteration changed.
//
// THE FIX — iterating a borrowing collection is not an escape: `for` clones a
// per-loop enumerator that BORROWS the backing and whose Dispose frees only its
// own struct (the same property #1871 / #2142 rely on to admit a for-in in
// StringEscapeWalker's `borrowingArrayMode`, and the reason the ROOT's own
// reclaim already fired for these programs). What needs bounding is the loop
// VARIABLE, which aliases an interior node for the body's extent. The walk now
// tracks it as exactly that, and holds it to the SAME whitelist as an interior
// node read out of the root: a bare mention (returned, bound to an outer local,
// passed, captured) denies; only a borrow position that cannot retain it — an
// index base, a scalar-read member (`Count`/`Length`/`IsEmpty`), or a further
// iteration — is admitted.
//
// ⚠ The dangerous direction here is a USE-AFTER-FREE, not a leak, and it is not
// hypothetical: #2660 shipped a depth-3 UAF on its first pass. The gate was
// therefore validated against a deliberately OVER-WIDE build (the iterable
// admitted but the loop variable NOT tracked). Under that build the
// `alias_returned_*` and `alias_bound_to_outer_local_*` controls below both die
// with `AddressSanitizer: SEGV in __memmove_avx_unaligned_erms`; under the
// shipped gate they leak by design and return the correct value. The controls
// are therefore known-discriminating, not merely green.
//
// Only a BARE identifier naming the root, or an already-live alias, is admitted
// as the iterable. Every other iterable that could yield interior nodes must
// mention the root in a producing position (`rows[k]`, `rows.Where(..)`, an
// alias binding) and expr() denies each of those, so an unadmitted iterable can
// never place an interior node in an untracked loop variable.
//
// fail-without/pass-with: the four reclaim tests leak on the pre-fix build (each
// `runAotChecked` flips `r.ran` false → -999) and are clean with the fix; the
// controls hold their pre-fix behaviour on both builds.
// per §15.4, §21.4, §28.9, §28.11 (issue #2711, spec v0.240)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// ASan + LSan: LSan proves the interior nodes were freed AT LEAST once (no
// leak), ASan proves AT MOST once (no double-free / no use-after-free). For a
// built-in collection node, whose reclaim runs no user code and so cannot be
// counted from the program, that pair IS the exactly-once oracle. Same helper
// shape as the #2660 file this extends.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// For the controls whose whole point is that the gate DECLINES to free the
// interior nodes: those programs leak BY DESIGN (the pre-fix behaviour,
// preserved deliberately — a bounded leak is the safe direction and a dangle is
// not). runAotChecked would fail on that expected leak and hide the real
// assertion, which is that the value is still correct. Each control therefore
// runs 300 allocation-churn iterations between the point the tree's scope ends
// and the point the escaped node is read, so a premature free is caught as a
// corrupted value on this NON-sanitized oracle too, not only under ASan.

// #5123 (§28.3 row 4, v0.313): a plain static return of a builtin collection
// class is rejected at the declaration. True iff `prog` fails to compile with
// exactly that diagnostic.
bool rejectedWithRow4(const std::string& prog) {
    auto r = runAot(prog);
    return !r.ran && bezel::test::diagnosticMessageContains(
        r, "returns class/interface type `Array` without `owned`");
}
int runValueOnly(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The issue's own repro, `using var` + nested iteration, over 20 scope
// entries. Three returns the summed length of the 3 stored strings: 3x3 for
// "v-1".."v-9" (9 entries) + 3x4 for "v-10".."v-20" (11 entries) = 81 + 132 =
// 213. Pre-fix: 4591 B / 140 allocations leaked.
// per §15.4, §21.4, §28.9 (issue #2711)
BEZEL_TEST(literal_tree_iterated_root_2711, using_var_iterated_interior_reclaim_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Three(s: string): int "
        "    using var rows: Array<LinkedList<string>> := [[s, s], [s]]; "
        "    var n := 0; "
        "    for var row in rows do "
        "      for var it in row do "
        "        n := n + it.Length; "
        "      end; "
        "    end; "
        "    return n; "
        "  end Three; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..20 do "
        "      total := total + C.Three($\"v-{i}\"); "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        213);
}

// ── The SAME tree and body with a plain `var` root. Bisecting the two axes of
// the issue's title separately is what showed `using var` is not the
// discriminator: this leg leaked the identical 4591 B / 140 allocations pre-fix.
// per §15.4, §21.4, §28.11 (issue #2711)
BEZEL_TEST(literal_tree_iterated_root_2711, plain_var_iterated_interior_reclaim_spec_28_11) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Three(s: string): int "
        "    var rows: Array<LinkedList<string>> := [[s, s], [s]]; "
        "    var n := 0; "
        "    for var row in rows do "
        "      for var it in row do "
        "        n := n + it.Length; "
        "      end; "
        "    end; "
        "    return n; "
        "  end Three; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..20 do "
        "      total := total + C.Three($\"v-{i}\"); "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        213);
}

// ── AMPLIFIED to 64 separate tree constructions, so reclaim is proven
// POSITIVELY rather than by the absence of a report: a stale-stack-slot LSan
// false negative can leave at most the LAST tree spuriously reachable, never all
// 64. The exit value is a function of every stored string's length across all 64
// trees, so it also pins that nothing was freed early. Per entry n = (2 + 1)
// counts + 3L; L=7 for "pos64-1".."pos64-9" (9 entries, n=24) and L=8 for
// "pos64-10".."pos64-64" (55 entries, n=27): 216 + 1485 = 1701, less 1536 = 165.
// per §15.4, §21.4, §28.9 (issue #2711)
BEZEL_TEST(literal_tree_iterated_root_2711, iterated_interior_reclaim_amplified_64_trees_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Three(s: string): int "
        "    using var rows: Array<LinkedList<string>> := [[s, s], [s]]; "
        "    var n := 0; "
        "    for var row in rows do "
        "      n := n + row.Count; "
        "      for var it in row do "
        "        n := n + it.Length; "
        "      end; "
        "    end; "
        "    return n; "
        "  end Three; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..64 do "
        "      total := total + C.Three($\"pos64-{i}\"); "
        "    end; "
        "    return total - 1536; "
        "  end Main; "
        "end C;"),
        165);
}

// ── The reclaim must still fire on the BREAK edge out of the loop and on an
// early RETURN — the interior disposer rides the root's dispose stack, so every
// scope exit must carry it. n = 2 (first row) then break on the 1-element row;
// 40 entries x 2 = 80.
// per §21.4, §28.9, §28.11 (issue #2711)
BEZEL_TEST(literal_tree_iterated_root_2711, iterated_reclaim_on_break_and_return_edges_spec_21_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Sum(s: string): int "
        "    using var rows: Array<LinkedList<string>> := [[s, s], [s]]; "
        "    var n := 0; "
        "    for var row in rows do "
        "      if row.Count = 1 then "
        "        break; "
        "      end; "
        "      n := n + row.Count; "
        "    end; "
        "    if n > 1 then "
        "      return n; "
        "    end; "
        "    return 0; "
        "  end Sum; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..40 do "
        "      total := total + C.Sum($\"e-{i}\"); "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        80);
}

// ── NEIGHBOUR (no regression): a loop variable over an unrelated source — a
// range that merely mentions the root in a scalar-read position — must NOT be
// treated as an interior alias, so the indexed body keeps working exactly as it
// did before. 40 entries x (2 + 1) = 120.
// per §21.4, §28.9 (issue #2711)
BEZEL_TEST(literal_tree_iterated_root_2711, range_loop_variable_is_not_an_alias_spec_21_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Sum(s: string): int "
        "    using var rows: Array<LinkedList<string>> := [[s, s], [s]]; "
        "    var n := 0; "
        "    for var i in 0..rows.Count - 1 do "
        "      n := n + rows[i].Count; "
        "    end; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..40 do "
        "      total := total + C.Sum($\"f-{i}\"); "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        120);
}

// ── CONTROL (leaks by design). RETURN the loop variable: a bare mention of an
// interior alias. Must decline — the node outlives the root's scope. This is one
// of the two controls that SEGV under an over-wide gate that admits the iterable
// without tracking the loop variable. got = [7,8,9]: 3*10 + (7-6) = 31.
// per §15.4, §28.6, §21.4 (issue #2711 self-pass)
BEZEL_TEST(literal_tree_iterated_root_2711, alias_returned_static_rejected_spec_28_3_row4) {
    BEZEL_EXPECT_TRUE(rejectedWithRow4(
        "module M; "
        "class C "
        "  public static method Get(): Array<int> "
        "    var deep: Array<Array<int>> := [[7, 8, 9], [4]]; "
        "    for var row in deep do "
        "      if row.Count > 2 then "
        "        return row; "
        "      end; "
        "    end; "
        "    return [0, 0, 0]; "
        "  end Get; "
        "  public static method Main(): int "
        "    var got := C.Get(); "
        "    var churn := 0; "
        "    for var i in 1..300 do "
        "      var tmp: Array<int> := [i, i, i, i]; "
        "      churn := churn + tmp.Length; "
        "    end; "
        "    return got.Count * 10 + (got[0] - 6); "
        "  end Main; "
        "end C;"));
}

// ── CONTROL (leaks by design). Bind the loop variable into an OUTER local that
// outlives the loop, and return that. The second control that SEGVs under the
// over-wide gate. Value 31.
// per §15.4, §28.6, §21.4 (issue #2711 self-pass)
BEZEL_TEST(literal_tree_iterated_root_2711, alias_bound_to_outer_local_static_rejected_spec_28_3_row4) {
    BEZEL_EXPECT_TRUE(rejectedWithRow4(
        "module M; "
        "class C "
        "  public static method Get(): Array<int> "
        "    var deep: Array<Array<int>> := [[7, 8, 9], [4]]; "
        "    var kept: Array<int> := [0, 0, 0]; "
        "    for var row in deep do "
        "      if row.Count > 2 then "
        "        kept := row; "
        "      end; "
        "    end; "
        "    return kept; "
        "  end Get; "
        "  public static method Main(): int "
        "    var got := C.Get(); "
        "    var churn := 0; "
        "    for var i in 1..300 do "
        "      var tmp: Array<int> := [i, i, i, i]; "
        "      churn := churn + tmp.Length; "
        "    end; "
        "    return got.Count * 10 + (got[0] - 6); "
        "  end Main; "
        "end C;"));
}

// ── CONTROL (leaks by design), DEPTH 3. INDEX a loop-variable alias and return
// the result. The index BASE is an admissible borrow position, so syntactic
// position alone would admit this — exactly the reasoning that produced #2660's
// shipped depth-3 UAF. It is the depth-independent RESULT-TYPE gate that denies
// it, and this control proves that gate composes with alias tracking rather than
// being bypassed by it. Value 31.
// per §15.4, §28.6, §21.4 (issue #2711 self-pass)
BEZEL_TEST(literal_tree_iterated_root_2711, alias_indexed_depth3_returned_static_rejected_spec_28_3_row4) {
    BEZEL_EXPECT_TRUE(rejectedWithRow4(
        "module M; "
        "class C "
        "  public static method Get(): Array<int> "
        "    var deep: Array<Array<Array<int>>> := [[[7, 8, 9]], [[4]]]; "
        "    for var mid in deep do "
        "      if mid.Count > 0 then "
        "        return mid[0]; "
        "      end; "
        "    end; "
        "    return [0, 0, 0]; "
        "  end Get; "
        "  public static method Main(): int "
        "    var got := C.Get(); "
        "    var churn := 0; "
        "    for var i in 1..300 do "
        "      var tmp: Array<int> := [i, i, i, i]; "
        "      churn := churn + tmp.Length; "
        "    end; "
        "    return got.Count * 10 + (got[0] - 6); "
        "  end Main; "
        "end C;"));
}

// ── CONTROL (leaks by design), DEPTH 3. The CHAINED read `deep[0][0]` returned
// out of a body that ALSO iterates the root — #2660's own shipped-UAF shape,
// re-pinned in the presence of an admitted iteration so the two gates cannot
// mask each other. got = [1,2,3]: 3*10 + 1 = 31.
// per §15.4, §28.6 (issue #2711 self-pass)
BEZEL_TEST(literal_tree_iterated_root_2711, chained_depth3_returned_static_rejected_spec_28_3_row4) {
    BEZEL_EXPECT_TRUE(rejectedWithRow4(
        "module M; "
        "class C "
        "  public static method Get(): Array<int> "
        "    var deep: Array<Array<Array<int>>> := [[[1, 2, 3]], [[4]]]; "
        "    var n := 0; "
        "    for var mid in deep do "
        "      n := n + mid.Count; "
        "    end; "
        "    return deep[0][0]; "
        "  end Get; "
        "  public static method Main(): int "
        "    var got := C.Get(); "
        "    var churn := 0; "
        "    for var i in 1..300 do "
        "      var tmp: Array<int> := [i, i, i, i]; "
        "      churn := churn + tmp.Length; "
        "    end; "
        "    return got.Count * 10 + got[0]; "
        "  end Main; "
        "end C;"));
}

// ── CONTROL (leaks by design). Condition (a) — the ROOT itself escapes into a
// call from a body that also iterates it. An admitted iteration must not
// override the root escape: the tree goes out with the root, so nothing here may
// free anything. 40 entries x (n=3 + rows.Count=2) = 200.
// per §15.4, §28.6 (issue #2711 self-pass)
BEZEL_TEST(literal_tree_iterated_root_2711, root_escapes_from_iterated_body_no_uaf) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class C "
        "  public static method Take(rows: Array<LinkedList<string>>): int "
        "    return rows.Count; "
        "  end Take; "
        "  public static method Sum(s: string): int "
        "    using var rows: Array<LinkedList<string>> := [[s, s], [s]]; "
        "    var n := 0; "
        "    for var row in rows do "
        "      n := n + row.Count; "
        "    end; "
        "    return n + C.Take(rows); "
        "  end Sum; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..40 do "
        "      total := total + C.Sum($\"g-{i}\"); "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        200);
}
