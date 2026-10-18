// Spec-derived regression test for #6352 — the DICT-backed borrowing
// collections' unbound owned temporaries are never reclaimed, so a
// `MakeSet().Count` / `MakeDict().Count` leaks its whole table.
//
// THE DEFECT. `emitCall`'s Array `.Length`/`.Count` arm reclaims an UNBOUND
// owned-producing receiver after reading the length/count, via
// `freeUnboundOwnedBorrowingArrayTemp` (#1006(b)) reached through the
// `freeFreshInputTemp` lambda. Its recognizer,
// `isUnboundOwnedBorrowingArrayCallExpr` (LlvmBackend_Setup.cpp), admitted
// exactly ONE type:
//
//     if (builtinTypeName(rt) != "Array") return false;
//
// `Set<T>`, `Dictionary<K,V>`, `Stack<T>`, `Queue<T>`, `LinkedList<T>`,
// `SortedSet<T>` and `SortedDictionary<K,V>` are all declaration-less builtin
// collection heads living in the SAME `kChainClaimedBuiltinHeadNames` set
// (BuiltinRegistry.cpp) as `Array`, and the Set/Dictionary storage IS the
// `bz_dict_t` the runtime already had a `bz_dict_free` for — so nothing about
// ownership classification distinguishes them. The recognizer simply stopped
// at the first name. The helper's own tail then unconditionally called
// `bz_array_free`, so even had the recognizer admitted them it would have
// freed a `bz_dict_t*` through the array entry point.
//
// Meanwhile `Array<T>` DOES reclaim — not because it is classified
// differently, but because it is the one head that recognizer names.
//
// SPEC. §28.3 *Unbound owned temporaries* requires the temporary an unbound
// owned-producing call's result binds to be reclaimed at the end of the
// enclosing expression statement. §28.9 *Runtime-managed types* declares its
// tier EXHAUSTIVE — "no runtime lifetime mechanism exists for user-defined
// classes at all" — and names only `string`, `Type`, boxed values and
// function-typed values. Collections are NOT in it, so `Set<T>`/`Dictionary<K,V>`
// are ordinary §28.3-disposable objects exactly like `Array<T>`; the missing
// call is a missing disposal, NOT a wrong ownership classification. stdlib
// §4.10 / §4.8 declare both as ordinary classes with a public constructor, and
// the §4.4 collection paragraph states `Array<T>`/`OwningArray<T>` "are class
// types allocated with `new` and requiring disposal via `using var`, owned
// field, or explicit `.Dispose()`" — the same applies to every sibling head.
//
// MEASURED on this fix's parent, this issue's own repro (`MakeSet().Count`
// × 200, plus one `MakeDict().Count` and one `MakeArr().Count`) under
// `BEZEL_AOT_FORCE_SANITIZE=address`: LSan reported
//
//   Direct leak of 19200 byte(s) in 200 object(s)      [bz_dict_alloc]
//   Indirect leak of 51200 byte(s) in 200 object(s)    [bz_dict_alloc_generic]
//   Direct leak of 96 byte(s) in 1 object(s)           [the Dictionary]
//   Indirect leak of 256 byte(s) in 1 object(s)
//   SUMMARY: AddressSanitizer: 70752 byte(s) leaked in 402 allocation(s)
//
// while the `MakeArr().Count` twin of the same loop reclaimed cleanly. The
// 402 allocations are 200 Set tables + 200 of their backing store + the
// Dictionary and its store: every single one of the Set/Dictionary
// temporaries, and NONE of the Array ones.
//
// THE FIX. `isUnboundOwnedBorrowingArrayCallExpr` now tests
// `common::isPlainBuiltinCollectionHead` — the SAME predicate that already
// enumerates the eight borrowing collection heads (BuiltinRegistry.cpp:35-38,
// :974) — instead of the hard-coded `"Array"`, so it admits every borrowing
// collection head uniformly. The Set `.Count` and `.IsEmpty` arms — which,
// unlike the Dictionary `.Count` arm, never called the helper at all — now call
// it. `freeUnboundOwnedBorrowingArrayTemp`'s tail dispatches on the receiver's
// actual backing: `bz_llist_free` for a `LinkedList`, `bz_dict_free` for a
// `Dictionary` or a generic-keyed `Set`/`SortedSet` (all `bz_dict_t`), and
// `bz_array_free` otherwise. The dispatch mirrors
// `freeUnboundOwnedOwningCollectionTemp` and `buildCollectionTempDisposal`,
// which already distinguish those three families at the same call sites.
// `SortedSet` needs its own name in that test: BuiltinRegistry deliberately
// keeps it out of `kDictionaryNames` while documenting that it "shares the
// generic-key hash table", so `isDictTypeName` alone would route a
// `SortedSet<string>` to `bz_array_free` — a wrong-free, not a missed free.
//
// §429's co-ownership constraint is respected on BOTH dict columns, which took
// two different shapes. A set-family `string` ELEMENT lives in the KEY slot
// with a co-owned +1 that `bz_dict_free` does not release (it only
// `free(e.key)`), so the fix calls #429's `bz_set_string_keys_release` —
// documented for exactly this, "Call BEFORE bz_dict_free" — and then frees the
// table. A string-KEYED `Dictionary<K,V>` DECLINES: its value column can be an
// owning type needing its own disposer and #429 shipped no whole-table helper
// for it, so guessing at a release contract here would be inventing one.
// `string_keyed_dict_declined_but_correct` pins that decline.
//
// The two dict columns are also why the fix passes a NULL element disposer on
// the dict path: `bz_dict_free`'s second argument applies to `e.val`, the
// VALUE column, which is not where a set-family element lives. A first draft
// that fell through with the array disposer still installed produced an ASan
// SEGV (`bz_string_release` on address 0x19, via `bz$strElemRelease` from
// `bz_dict_free`) — the disposer aimed at the wrong column. That measurement
// is what the `elemDisp` comment in the fix records.

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The common preamble. §28.3's unbound-owned-temporary shape is an `owned`
// return type consumed ONLY as a member access's receiver — a `var` or an
// `owned var` would make it BOUND and thus the callee's own responsibility
// (§28.3's second case, already correct), which is why every producer below
// returns `owned` and no call site binds the result.
const char* kPreamble =
    "module T; "
    "public class C "
    "  public static method MakeSet(): owned Set<int> "
    "    return new Set<int>(); "
    "  end MakeSet; "
    "  public static method MakeArr(): owned Array<int> "
    "    return new Array<int>(); "
    "  end MakeArr; "
    // `Main` is appended per-row INSIDE this class body — the preamble
    // deliberately leaves the class OPEN (no `end C;`) so each row supplies
    // its own body and its own terminator.
    "  ";

// 200 iterations, per this repo's one-shot-leak-repro discipline: a single
// iteration's table is a handful of allocations that a plausible per-program
// one-off budget could absorb. The loop makes "every iteration leaks" show up
// as a count that scales.
const int kIters = 200;

// §28.3 / §28.9 / stdlib §4.10 / #6352 (v0.338): the issue's own repro at
// 200 iterations. Pre-fix, LSan reports 70752 B / 402 allocs and `runAotChecked`
// gives `r.ran == false`; post-fix the loop's own exit status comes back
// clean. The `MakeArr().Count` line is the same loop's Array twin and is
// here as the CONTROL: it already reclaimed pre-fix, so if the fix regressed
// the Array path this row would go red for a different reason.
//
// `MakeSet()` is `Set<int>` and `MakeDict()` is `Dictionary<int, int>`: the
// value-typed half of the dict-backed family. `Dictionary<string, V>` is a
// DIFFERENT shape and is deliberately NOT reclaimed by this fix — see
// `string_keyed_dict_still_declined` below, which pins the decline.
BEZEL_TEST(collection_temp_reclaim_count_6352,
           issue_repro_200_iterations_reclaims) {
    const char* valPreamble =
        "module T; "
        "public class C "
        "  public static method MakeSet(): owned Set<int> "
        "    return new Set<int>(); "
        "  end MakeSet; "
        "  public static method MakeDict(): owned Dictionary<int, int> "
        "    return new Dictionary<int, int>(); "
        "  end MakeDict; "
        "  public static method MakeArr(): owned Array<int> "
        "    return new Array<int>(); "
        "  end MakeArr; "
        "  ";
    std::string src = std::string(valPreamble) +
        "  public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < " + std::to_string(kIters) + " do "
        "    acc := acc + C.MakeSet().Count; "
        "    i := i + 1; "
        "  end while; "
        "  acc := acc + C.MakeDict().Count; "
        "  acc := acc + C.MakeArr().Count; "
        "  return acc; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}

// §28.3 / §28.9 / stdlib §4.10 / #6352 (v0.338): the `Set<string>` case, whose
// element carries a co-owned +1 that `bz_dict_free` does not release (it only
// `free(e.key)`). #429 shipped `bz_set_string_keys_release` for exactly this
// shape — "releases every live element's +1 by reading the string pointer from
// each key slot. Call BEFORE bz_dict_free" — and this fix calls it before the
// table free. An EMPTY set has no live keys, so this row's table leak (the
// §28.3 half) is what it measures; the string-release half is exercised by
// `set_string_element_keys_released`, which puts elements in.
BEZEL_TEST(collection_temp_reclaim_count_6352,
           set_string_unbound_owned_reclaims) {
    const char* strSetPreamble =
        "module T; "
        "public class C "
        "  public static method MakeSet(): owned Set<string> "
        "    return new Set<string>(); "
        "  end MakeSet; "
        "  ";
    std::string src = std::string(strSetPreamble) +
        "  public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < " + std::to_string(kIters) + " do "
        "    acc := acc + C.MakeSet().Count; "
        "    i := i + 1; "
        "  end while; "
        "  return acc; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}

// §28.3 / §28.9 / stdlib §4.10 / #6352 (v0.338): `Set<string>` WITH live
// elements, so the per-key string release is exercised rather than just the
// empty-table path. #429's helper reads the key slot as a `bz_string*`, which
// is what makes the set-family gate load-bearing — if this fix widened
// `bz_set_string_keys_release` to a `Dictionary`, whose key column is freed but
// whose VALUE column can be an owning type, this row is the one that would go
// red for it.
BEZEL_TEST(collection_temp_reclaim_count_6352,
           set_string_element_keys_released) {
    const char* strSetPreamble =
        "module T; "
        "public class C "
        "  public static method MakeSet(n: string): owned Set<string> "
        "    var s := new Set<string>(); "
        "    s.Add(n); "
        "    return s; "
        "  end MakeSet; "
        "  ";
    std::string src = std::string(strSetPreamble) +
        "  public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < " + std::to_string(kIters) + " do "
        "    acc := acc + C.MakeSet(\"k\").Count; "
        "    i := i + 1; "
        "  end while; "
        "  return acc; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Each iteration's set holds exactly the one element added, so the
    // accumulated count is the iteration count — which also proves the element
    // really was stored (and so really was a live key slot that
    // `bz_set_string_keys_release` had to walk), not an empty table.
    BEZEL_EXPECT_TRUE(r.exitCode == kIters);
}

// §28.3 / §28.9 / #6352 (v0.338): the same unbound-owned-temporary shape read
// through `.IsEmpty()` instead of `.Count`. Both are pure READS of the table
// — neither can be observed for its value without materializing the receiver
// — and pre-fix both arms returned straight out of the `bz_dict_count` runtime
// call. Kept as its own row so the reclaim is pinned on the member, not only on
// the call site that happens to host it today.
BEZEL_TEST(collection_temp_reclaim_count_6352,
           is_empty_unbound_owned_set_reclaimed) {
    std::string src = std::string(kPreamble) +
        "  public static method Main(): int "
        "  var i := 0; "
        "  while i < " + std::to_string(kIters) + " do "
        "    if C.MakeSet().IsEmpty() then Console.WriteLine(1); end if; "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}

// §28.3 / §28.9 / #6352 (v0.338): the BOUND-receiver twin of the row above,
// as the double-free / use-after-free guard. Here the set is bound to a
// `using var`, so the §28.3 temporary rule does not apply — the BLOCK owns it
// and disposes it at scope end. If the fix's reclaim fired on a bound
// receiver, this row would read freed memory (ASan use-after-free) or
// double-free at scope exit. It must stay clean, and it must still see its
// own data (the `Contains` result is asserted), which a premature free would
// break independently of ASan.
BEZEL_TEST(collection_temp_reclaim_count_6352,
           bound_set_still_disposed_exactly_once) {
    std::string src = std::string(kPreamble) +
        "  public static method Main(): int "
        "  using var s := C.MakeSet(); "
        "  s.Add(7); "
        "  if s.Contains(7) then Console.WriteLine(1); end if; "
        "  return s.Count; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 1);
}

// §28.3 / §28.9 / #6352 (v0.338): a BOUND alias of an unbound-producing call
// read through a member access. `var s := C.MakeSet()` makes the §28.3
// temporary BOUND to `s`, so `s.Count` must NOT reclaim a second time — the
// fix's recognizer is required to decline here, and this row is what proves
// the recognizer keys on the UNBOUND shape rather than merely on the type.
BEZEL_TEST(collection_temp_reclaim_count_6352,
           aliased_bound_set_not_double_freed) {
    std::string src = std::string(kPreamble) +
        "  public static method Main(): int "
        "  var s := C.MakeSet(); "
        "  var n := s.Count; "
        "  s.Add(1); "
        "  n := n + s.Count; "
        "  return n; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // 0 from the empty set, then 1 after `Add(1)` — the block still sees and
    // mutates its own set, which a premature (fix-induced) reclaim would break
    // independently of ASan.
    BEZEL_EXPECT_TRUE(r.exitCode == 1);
}

// §28.3 / §28.9 / stdlib §4.8 / #6352 (v0.338): the ONE shape this fix
// declines, pinned so the decline cannot be widened by accident.
//
// `Dictionary<string, V>`'s string lives in the KEY column. `bz_dict_free`
// frees that key (`free(e.key)`) without releasing its +1 — the #429 problem
// again — but unlike a set, the VALUE column can be an owning type that needs
// its own disposer, so #429 shipped NO whole-table helper for it (only
// `bz_dict_clear_set_strings` / `bz_dict_remove_set_string`, both
// set-specific). Emitting `bz_set_string_keys_release` here would read the key
// slot as a `bz_string*` and release it — correct for the key — while silently
// dropping the value column's own disposer. Whether `bz_dict_free` should
// release string keys itself, or whether a `bz_dict_free_strings` belongs
// beside it, is a RUNTIME + §28.9 co-ownership-contract question that this fix
// does not settle and must not guess at; it is the follow-up recorded in the
// handback.
//
// So this row asserts only what IS true post-fix and is what the DECLINE buys:
// the program compiles, runs, and produces the right value — no wrong-free,
// no double-free, no crash. It therefore uses `runAot`, NOT `runAotChecked`:
// `runAotChecked` is always-ASan, so a row pinning a KNOWN leak through it
// could never be green, and weakening the oracle to accommodate one would be
// exactly the kind of test-weakening this repo's protocol forbids. The known
// leak is real and is reported in this fix's handback as the follow-up, not
// pinned as a pass here — this row's job is to hold the DECLINE line, and a
// `runAotChecked` leak oracle would turn that into a tautology.
BEZEL_TEST(collection_temp_reclaim_count_6352,
           string_keyed_dict_declined_but_correct) {
    const char* strDictPreamble =
        "module T; "
        "public class C "
        "  public static method MakeDict(): owned Dictionary<string, int> "
        "    return new Dictionary<string, int>(); "
        "  end MakeDict; "
        "  ";
    std::string src = std::string(strDictPreamble) +
        "  public static method Main(): int "
        "  var i := 0; "
        "  while i < 2 do "
        "    if C.MakeDict().Count = 0 then i := i + 1; end if; "
        "  end while; "
        "  return 7; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 7);
}

}  // namespace