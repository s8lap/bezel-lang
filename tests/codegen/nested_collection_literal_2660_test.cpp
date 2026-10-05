// Codegen — issue #2660 (§15.4 collection literals, §28.9 runtime-managed /
// trivial-dispose types, §28.11 borrowing collections; spec v0.237): a NESTED
// collection literal did not lower at all.
//
// §15.4's own motivating examples — `Dictionary<string, Array<int>> :=
// ["primes": [2,3,5,7], …]` and `Dictionary<string, Dictionary<string,int>> :=
// ["server": ["port": 8080, …], …]`, carried verbatim in the conformance corpus
// at tests/conformance/cases/spec_0060.bzl:15-24 — failed to compile with
// `internal: lowerExpr returned null for dictionary literal key or value —
// #1386`, one per nested value, in BOTH the plain-`var` and `using var` forms.
// (The `#1386` tag names the closed hardening sweep that ADDED that internal-
// error guard at 27 codegen sites, not an open tracker.) The symmetric
// sequence-side shape `Array<Dictionary<string,int>> := [["a": 1]]` failed the
// same way through the sibling guard, "…for heap collection literal element".
//
// Root cause: `lowerExpr` returns null for a `CollectionLiteralExpr` (a literal
// is not a general expression value — it needs a target type), so every
// materializer must recognise a nested literal itself. The sequence-literal
// materializer did, but only for a nested SEQUENCE element (#465, guarded
// `!nested->isDict`); `materializeHeapDictLiteral` had no nested handling at
// all and fell straight into the #1386 guard.
//
// Both now route through `materializeCollectionLiteralForArg`, the dispatcher
// that already fans a literal out to the dict / array / set / linked-list
// materializers — so an interior node is bit-for-bit what `new` + `.Add` builds.
//
// SECOND HALF — the interior nodes' reclaim. Making the shape compile is not
// the whole fix: each interior node is its own heap collection with its own
// backing. §15.4 (v0.237) is explicit about the shape's well-formedness and its
// storage rule: "A fresh producer of trivial-dispose class type remains
// admissible: it has no disposal obligation to dangle — its storage is
// reclaimed with the instance by [§28]'s implementation-defined mechanism,
// which runs no user code and is invisible to programs — which is what keeps
// this section's own nested-literal examples well-formed: in the `groups` and
// `config` examples below, the nested `Array<int>` and `Dictionary<string,
// int>` values are fresh producers of trivial-dispose type ([§28.9])."
// The flat reclaim family (#1871/#2142/#2143) frees only the ROOT, so every
// interior node leaked (measured on the pre-fix build: 224 B for `groups`,
// 837 B for `config`, and 4480 B over the 20-iteration array-of-arrays loop
// below). The root's existing scope-end free now carries an interior-node
// disposer, but ONLY for a root proven to satisfy all three of:
//   (a) the root never escapes (the same StringEscapeWalker the flat family
//       uses);
//   (b) every node of the tree is a fresh nested literal, recursively — a
//       BORROWED member is never an aggregate member (proposal 0077 §3) and
//       freeing it would double-free the binding that owns it;
//   (c) no INTERIOR node read out of the root can outlive the root's scope
//       (a new default-deny walk — StringEscapeWalker tracks only the root
//       identifier and classifies `var inner := groups["primes"];` as a safe
//       read of `groups`).
// A root failing any of the three keeps the pre-fix behaviour exactly: the
// interior nodes leak. That is deliberate — a bounded leak, never a dangle.
// The five self-pass controls at the bottom pin that boundary.
//
// fail-without/pass-with: the lowering tests do not compile at all without the
// fix (hard ICE); the two LOOPED tests pin the interior reclaim specifically —
// `array_of_arrays_interior_reclaim_looped_spec_28_9` compiles on the pre-fix
// build (the #465 path) and leaks 4480 B / 80 allocations there.
// per §15.4, §28.9, §28.11 (issue #2660, spec v0.237)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// ASan + LSan: LSan proves the node was freed AT LEAST once (no leak) and ASan
// proves it was freed AT MOST once (no double-free / no use-after-free). For a
// built-in collection node, whose reclaim runs no user code and so cannot be
// counted from the program, that pair IS the exactly-once oracle.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// For the self-pass controls whose whole point is that the fix DECLINES to free
// the interior nodes: those programs leak BY DESIGN (the pre-fix behaviour,
// preserved). runAotChecked would fail on that expected leak and hide the real
// assertion, which is that the value is still correct — i.e. no UAF and no
// double-free. runAot skips ASan entirely, so only a genuine crash flips `ran`.
// (Same rationale, and same helper shape, as the #2142/#2143 escape controls.)

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

// ── §15.4's `groups` example, plain `var`. Nested sequence literal in dictionary
// VALUE position — the issue's exact repro. Value: groups["primes"][2] +
// groups["evens"][3] = 5 + 8 = 13. // per §15.4 (issue #2660)
BEZEL_TEST(nested_collection_literal_2660, dict_of_arrays_plain_var_spec_15_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var groups: Dictionary<string, Array<int>> := "
        "      [\"primes\": [2, 3, 5, 7], \"evens\": [2, 4, 6, 8]]; "
        "    return groups[\"primes\"][2] + groups[\"evens\"][3]; "
        "  end Main; "
        "end C;"),
        13);
}

// ── The same example in the `using var` form the spec and the conformance
// corpus actually write (spec_0060.bzl:15). A `using var` root's disposal comes
// from registerUsingDisposal, a DIFFERENT registration site than the plain-`var`
// path above, so both needed the interior disposer. Value: 13.
// per §15.4, §28.10 (issue #2660)
BEZEL_TEST(nested_collection_literal_2660, dict_of_arrays_using_var_spec_15_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var groups: Dictionary<string, Array<int>> := "
        "      [\"primes\": [2, 3, 5, 7], \"evens\": [2, 4, 6, 8]]; "
        "    return groups[\"primes\"][2] + groups[\"evens\"][3]; "
        "  end Main; "
        "end C;"),
        13);
}

// ── §15.4's `config` example (spec_0060.bzl:21), plain `var`: a nested
// DICTIONARY literal in dictionary value position — the interior node is
// bz_dict_t-backed, a different runtime free from the array case above.
// Value: config["client"]["retries"] + config["server"]["timeout"] = 3 + 30 = 33.
// per §15.4 (issue #2660)
BEZEL_TEST(nested_collection_literal_2660, dict_of_dicts_plain_var_spec_15_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var config: Dictionary<string, Dictionary<string, int>> := "
        "      [\"server\": [\"port\": 80, \"timeout\": 30], "
        "       \"client\": [\"retries\": 3]]; "
        "    return config[\"client\"][\"retries\"] + config[\"server\"][\"timeout\"]; "
        "  end Main; "
        "end C;"),
        33);
}

// ── `config`, `using var` form. Value: 33. // per §15.4, §28.10 (issue #2660)
BEZEL_TEST(nested_collection_literal_2660, dict_of_dicts_using_var_spec_15_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var config: Dictionary<string, Dictionary<string, int>> := "
        "      [\"server\": [\"port\": 80, \"timeout\": 30], "
        "       \"client\": [\"retries\": 3]]; "
        "    return config[\"client\"][\"retries\"] + config[\"server\"][\"timeout\"]; "
        "  end Main; "
        "end C;"),
        33);
}

// ── SEQUENCE-side symmetry: a nested DICTIONARY element of a sequence literal
// (`Array<Dictionary<string,int>>`). The #465 nested-element path declined this
// shape outright (`if (!nested->isDict …)`), so it hit the sibling #1386 guard
// "…for heap collection literal element". Value: rows[0]["b"] + rows[1]["c"] =
// 2 + 30 = 32. // per §15.4 (issue #2660)
BEZEL_TEST(nested_collection_literal_2660, array_of_dicts_spec_15_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var rows: Array<Dictionary<string, int>> := "
        "      [[\"a\": 1, \"b\": 2], [\"c\": 30]]; "
        "    return rows[0][\"b\"] + rows[1][\"c\"]; "
        "  end Main; "
        "end C;"),
        32);
}

// ── DEPTH-3 tree: `Dictionary<string, Dictionary<string, Array<int>>>`. The
// interior-node thunk recurses through the middle level, so the leaf arrays are
// freed by the middle dict's own free, which is in turn the root's value
// disposer. Value: deep["x"]["a"][1] + deep["y"]["c"][2] = 2 + 6 = 8.
// per §15.4, §28.9 (issue #2660)
BEZEL_TEST(nested_collection_literal_2660, depth_three_nesting_spec_15_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var deep: Dictionary<string, Dictionary<string, Array<int>>> := "
        "      [\"x\": [\"a\": [1, 2], \"b\": [3]], \"y\": [\"c\": [4, 5, 6]]]; "
        "    return deep[\"x\"][\"a\"][1] + deep[\"y\"][\"c\"][2]; "
        "  end Main; "
        "end C;"),
        8);
}

// ── RUNTIME-MANAGED leaf elements: the interior `Array<string>` co-owns a +1
// per element (#429 / §28.9), so its free must run the string-release thunk —
// a different disposer arm from the null-disposer int case. Under-releasing
// leaks (LSan) and over-releasing is an invalid free (ASan); runChecked pins
// both. Value: "beta".Length + "gamma".Length = 4 + 5 = 9.
// per §15.4, §28.9, §4.6 (issue #2660)
BEZEL_TEST(nested_collection_literal_2660, inner_string_elements_release_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var names: Dictionary<string, Array<string>> := "
        "      [\"a\": [\"alpha\", \"beta\"], \"b\": [\"gamma\"]]; "
        "    return names[\"a\"][1].Length + names[\"b\"][0].Length; "
        "  end Main; "
        "end C;"),
        9);
}

// ── EXACTLY-ONCE, looped. A single-shot LSan run can miss a leaked collection:
// LSan is a conservative stack scanner and a stale pointer-shaped word left on
// an unused stack slot keeps the block "reachable" (the false negative recorded
// on #2143). Twenty construct-and-drop iterations put the accumulated interior
// nodes far past what any one stale slot could mask. Value: 20 * (2 + 3) = 100.
// per §15.4, §28.9, §28.11 (issue #2660)
BEZEL_TEST(nested_collection_literal_2660, dict_of_arrays_interior_reclaim_looped_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..20 do "
        "      var groups: Dictionary<string, Array<int>> := "
        "        [\"a\": [1, 2], \"b\": [3]]; "
        "      total := total + groups[\"a\"][1] + groups[\"b\"][0]; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        100);
}

// ── The interior reclaim in ISOLATION. `Array<Array<int>>` already LOWERED
// before this fix (the #465 path), so this test's only new content is the
// interior-node free — it is the fail-to-pass witness for the reclaim half
// rather than the lowering half. Measured on the pre-fix build: 4480 byte(s)
// leaked in 80 allocation(s) (20 iterations x 2 arrays x {spine, data}).
// Value: 20 * (2 + 3) = 100. // per §15.4, §28.9, §28.11 (issue #2660)
BEZEL_TEST(nested_collection_literal_2660, array_of_arrays_interior_reclaim_looped_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..20 do "
        "      var grid: Array<Array<int>> := [[1, 2], [3, 4]]; "
        "      total := total + grid[0][1] + grid[1][0]; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        100);
}

// ══ SELF-PASS CONTROLS ══════════════════════════════════════════════════════
// Each of the five below is a shape where the interior free MUST be declined.
// They are the reason the fix carries an eligibility gate at all: every one of
// them compiles today with no diagnostic, so a blanket interior free would turn
// a leak into a double-free or a dangling read. They assert VALUE correctness
// under runAot (no ASan) — see the runValueOnly note at the top of the file.

// ── (b) MIXED literal: one fresh nested node and one BORROW of a named local.
// Proposal 0077 §3: "borrowed elements … are never members and are never freed
// with it" — `cached` is owned by its own binding, so freeing it through the
// dictionary would double-free. The whole tree is therefore declined.
// Value: groups["a"][0] + groups["b"][1] = 1 + 8 = 9.
// per §15.4, §28.11 (issue #2660 self-pass)
BEZEL_TEST(nested_collection_literal_2660, mixed_literal_borrowed_member_no_double_free) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var cached: Array<int> := [7, 8]; "
        "    var groups: Dictionary<string, Array<int>> := "
        "      [\"a\": [1, 2], \"b\": cached]; "
        "    return groups[\"a\"][0] + groups[\"b\"][1]; "
        "  end Main; "
        "end C;"),
        9);
}

// ── (c) `Add` AFTER construction. Proposal 0077 §3, resolution 5: "Aggregate membership
// never grows." The added value is a borrow the dictionary does not own, and
// the interior disposer is per-value at the runtime level — it cannot tell the
// literal's own nodes from a later arrival — so a receiver-of-`Add` root is
// declined wholesale. Value: 1 + 8 = 9.
// per §15.4, §28.11 (issue #2660 self-pass)
BEZEL_TEST(nested_collection_literal_2660, add_after_construction_no_double_free) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var cached: Array<int> := [7, 8]; "
        "    var groups: Dictionary<string, Array<int>> := [\"a\": [1, 2]]; "
        "    groups.Add(\"b\", cached); "
        "    return groups[\"a\"][0] + groups[\"b\"][1]; "
        "  end Main; "
        "end C;"),
        9);
}

// ── (c) An INTERIOR node bound to a named local and RETURNED. This shape
// compiles with no diagnostic today (verified on this fix's build), and it is
// precisely what the existing StringEscapeWalker cannot see: it treats
// `groups["primes"]` as a safe READ of `groups`. Freeing the interior node at
// `Leak`'s scope end would hand `Main` a dangling `Array<int>`. Declined —
// the node leaks instead, which is the safe direction.
//
// The ALLOCATION CHURN between the return and the read is load-bearing, not
// decoration: without it a freed-but-still-intact block reads back its old
// contents and the test would pass even if the gate were widened to admit this
// shape — a control that cannot fail is not a control. Building two fresh
// collections in between reuses the freed block, so a dangling read becomes a
// wrong value or a fault. (Demonstrated on the depth-3 sibling below, whose
// identical churn turned a widened gate into a hard ASan SEGV.)
// Value: a[0] + a[2] + sink = 2 + 5 + (9 + 8) = 24.
// per §15.4, §28.6 (issue #2660 self-pass)
BEZEL_TEST(nested_collection_literal_2660, interior_alias_returned_static_rejected_spec_28_3_row4) {
    BEZEL_EXPECT_TRUE(rejectedWithRow4(
        "module M; "
        "class C "
        "  public static method Leak(): Array<int> "
        "    var groups: Dictionary<string, Array<int>> := [\"primes\": [2, 3, 5]]; "
        "    var inner := groups[\"primes\"]; "
        "    return inner; "
        "  end Leak; "
        "  public static method Main(): int "
        "    var a := C.Leak(); "
        "    var churn: Dictionary<string, Array<int>> := "
        "      [\"p\": [9, 9, 9], \"q\": [8, 8]]; "
        "    var sink := churn[\"p\"][0] + churn[\"q\"][1]; "
        "    return a[0] + a[2] + sink; "
        "  end Main; "
        "end C;"));
}

// ── (c) DEPTH-3 interior alias returned — the shape that sent the first
// version of this fix back. `return deep["x"]["a"];` produces an interior node
// from the OUTER index, whose target is another index rather than the root
// identifier. A gate that decides by syntactic position ("the target of a
// further index is a borrow") is sound only at depth 2 and silently admitted
// this, after which the aggregate free reclaimed the returned `Array<int>`
// under the caller. Measured on that version: ASan SEGV in __memmove, i.e. a
// hard use-after-free, not a quiet bad read — the churn below is what makes it
// present that way. The gate now decides by the index expression's RESULT
// TYPE, which is depth-independent.
// Value: got[0] + got[2] + sink = 2 + 5 + (9 + 8) = 24.
// per §15.4, §28.6 (issue #2660 self-pass)
BEZEL_TEST(nested_collection_literal_2660, depth_three_interior_alias_returned_static_rejected_spec_28_3_row4) {
    BEZEL_EXPECT_TRUE(rejectedWithRow4(
        "module M; "
        "class C "
        "  public static method Leak(): Array<int> "
        "    var deep: Dictionary<string, Dictionary<string, Array<int>>> := "
        "      [\"x\": [\"a\": [2, 3, 5]]]; "
        "    return deep[\"x\"][\"a\"]; "
        "  end Leak; "
        "  public static method Main(): int "
        "    var got := C.Leak(); "
        "    var churn: Dictionary<string, Array<int>> := "
        "      [\"p\": [9, 9, 9], \"q\": [8, 8]]; "
        "    var sink := churn[\"p\"][0] + churn[\"q\"][1]; "
        "    return got[0] + got[2] + sink; "
        "  end Main; "
        "end C;"));
}

// ── DEPTH-3 READ still works. The type-based gate must not over-deny: a
// chained index whose OWN result is a scalar (`deep["x"]["a"][1]`) is still a
// borrow chain and keeps the aggregate eligible. Without this, the depth fix
// could have been "deny every chained index" and nothing would have caught it.
// runChecked: this one must be leak-free, unlike the two controls above.
// Value: deep["x"]["a"][1] + deep["y"]["c"][2] = 2 + 6 = 8.
// per §15.4, §28.9 (issue #2660 self-pass)
BEZEL_TEST(nested_collection_literal_2660, depth_three_chained_scalar_read_still_reclaims) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var deep: Dictionary<string, Dictionary<string, Array<int>>> := "
        "      [\"x\": [\"a\": [1, 2], \"b\": [3]], \"y\": [\"c\": [4, 5, 6]]]; "
        "    return deep[\"x\"][\"a\"][1] + deep[\"y\"][\"c\"][2]; "
        "  end Main; "
        "end C;"),
        8);
}

// ── #2682 boundary: an interior `Dictionary<K,string>` node is freed with a
// NULL child disposer, so its backing is reclaimed but the string's +1 is left
// alone. That is a deliberate bounded leak (hence runValueOnly): pairing a
// release with a construction site that takes no retain is what makes two
// containers sharing one runtime-built string double-free (#2682, pre-existing
// and reachable with no nesting at all). The point pinned here is that the
// program RUNS and is correct — before the narrowing this exact shape, with a
// flat container sharing the string, was an ASan Deallocate SEGV.
// NOTE the string must be runtime-built: "ab" + "cd" constant-folds to a
// sentinel on which release is a no-op, and the probe passes vacuously.
// Value: 20 iterations of ("v-1".."v-9" = 3, "v-10".."v-20" = 4) counted twice
// = 2 * (9*3 + 11*4) = 142. // per §15.4, §28.9 (issue #2660 / #2682)
BEZEL_TEST(nested_collection_literal_2660, interior_string_valued_dict_node_no_double_free) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..20 do "
        "      var s: string := $\"v-{i}\"; "
        "      var flat: Dictionary<string, string> := [\"k\": s]; "
        "      var nested: Dictionary<string, Dictionary<string, string>> := "
        "        [\"a\": [\"b\": s]]; "
        "      total := total + flat[\"k\"].Length + nested[\"a\"][\"b\"].Length; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        142);
}

// ── (c) `for var kv in groups do` binds each interior node to `kv.Value` —
// declined. Value: 2 + 1 = 3.
// NB (issue #2711): the REASON changed, the behaviour did not. #2660 declined
// this because it could not bound the loop VARIABLE at all, so every iterated
// root was denied; #2711 now bounds the loop variable as a tracked interior
// alias, and this shape still declines one step later — `kv.Value` is not one of
// the alias's admitted borrow positions (`Count`/`Length`/`IsEmpty`), so the
// dictionary-iteration shape keeps leaking by design. Tracked as #2740.
// per §15.4, §29.1 (issue #2660 self-pass)
BEZEL_TEST(nested_collection_literal_2660, for_in_over_root_no_uaf) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var groups: Dictionary<string, Array<int>> := "
        "      [\"a\": [1, 2], \"b\": [3]]; "
        "    var total := 0; "
        "    for var kv in groups do "
        "      total := total + kv.Value.Count; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        3);
}

// ── (a) The ROOT itself escapes via `return`. The flat escape walk already
// excludes such a root from its own reclaim; the interior free must follow it
// out, or the returned tree's nodes would be freed under the caller.
// Value: got["a"][1] + got["b"][1] = 2 + 4 = 6.
// per §15.4, §28.6 (issue #2660 self-pass)
BEZEL_TEST(nested_collection_literal_2660, root_escapes_via_return_no_uaf) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class C "
        "  public static method Make(): owned Dictionary<string, Array<int>> "
        "    var groups: Dictionary<string, Array<int>> := "
        "      [\"a\": [1, 2], \"b\": [3, 4]]; "
        "    return groups; "
        "  end Make; "
        "  public static method Main(): int "
        "    var got := C.Make(); "
        "    return got[\"a\"][1] + got[\"b\"][1]; "
        "  end Main; "
        "end C;"),
        6);
}
