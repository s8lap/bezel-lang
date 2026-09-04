// Codegen / SEGV — fix #3695 (§13.4 / §5 / §29.2 / §28.9, v0.286):
// `SortedSet<T>.GetEnumerator()` accessed through an `IEnumerable<T>`-typed
// variable SEGV'd, for every element type.
//
// THE DEFECT. `wrapAsIEnumerableForTarget` (`LlvmBackend_Assign.cpp`) is the
// §29.2 enumerable-wrap grid's authority, and `lowerLocalDecl`'s iterator-
// format-typed-init arm (`LlvmBackend_Setup.cpp`) is its sibling at the local-
// decl face (#3419 keeps both widening together). Both dispatch on the SOURCE
// family: `isArrayEnumeratorWrapSourceName` (Array / OwningArray / OwningBuffer
// / Stack / OwningStack / Queue / OwningQueue), `Set` / `OwningSet`
// (split by `genericBackingType` into class-element vs dict-backed snapshot),
// `isDictTypeName` (Dictionary / OwningDictionary / SortedDictionary). `SortedSet`
// hit none of those and fell through to the bare `return v` — the raw
// `bz_dict_t*` was handed to the caller.
//
// `en.GetEnumerator()` (where `en: IEnumerable<int>`) then drives the §29.11
// iterator vtable, reads slot 4 as a function pointer and calls it. On a
// `bz_dict_t` the first word is not a vtable — measured as a deterministic
// SIGSEGV (rc 139) with ASan reporting a null-address call (`pc 0x0`), the
// fault shape of an indirect call through a null or garbage function-pointer
// slot.
//
// The same family of bug as #3519 (Dictionary), #3419 (the grid itself), and
// the older #1069 (Set) — but the SortedSet arm was DELIBERATELY OMITTED from
// the original Set arm because yielding a sorted set's elements unordered
// would silently break stdlib §4.18's ordered-iteration contract (see the
// comment at `LlvmBackend_Assign.cpp:7744`). So #1069 et al. did not extend
// to SortedSet — and nothing else did, until this commit.
//
// MEASURED on `e89658c` (the r12 close-out HEAD), every row of this file:
//   LOCAL-DECL  `var en: IEnumerable<int> := s;` then `en.GetEnumerator()`
//               -> SIGSEGV, rc 139, `pc 0x0`
//   ARGUMENT    `Sum(s)` where the parameter is `IEnumerable<int>` and the
//               callee calls `.GetEnumerator()` -> SIGSEGV, rc 139
//   RETURN      `static method Mk(): IEnumerable<int> = s.GetEnumerator()`
//               -> SIGSEGV, rc 139
//   IN PLACE    `for var x in s do`                      -> n=47, rc 0
//   DIRECT      `s.GetEnumerator()` directly on concrete -> n=30, rc 0
//
// The direct / for-in surfaces route through `wrapSortedSetAsIEnumerable`
// (`LlvmBackend_Calls2.cpp:14146`); this fix extends the §29.2 coercion +
// local-decl init + argument/return coercion to route through the SAME
// helper, so the interface-upcast path is structurally identical to the direct
// path rather than a separately (and incorrectly) re-derived one.
//
// THE FIX. Add the `SortedSet` arm to (a) `wrapAsIEnumerableForTarget` (the
// RETURN / ARGUMENT / property-getter face) and (b) `lowerLocalDecl` (the
// LOCAL-DECL face), mirroring the SortedDictionary sibling. No new predicate
// — `builtinTypeName(initTR) == "SortedSet"` is sufficient, just like the
// SortedDictionary sibling's `builtinTypeName(initTR) == "SortedDictionary"`
// check at `LlvmBackend_Setup.cpp:14310` and `LlvmBackend_Assign.cpp:7924`.
//
// OWNERSHIP. `wrapSortedSetAsIEnumerable` returns an OWNING enumerator over a
// fresh sorted snapshot (`sortedSetSnapshotToSortedArray` →
// `wrapOwnedArrayAsIEnumerable`) — the wrapper's slot-1 Dispose frees the
// snapshot's array exactly once at the caller's scope end. The snapshot holds
// +1 references to the source's keys (a string slot is the SortedSet's own
// `bz_string*` +1, exactly as `bz_lazy_from_set` arranges); releasing the
// source via `bz_dict_free` decrements those refs and the snapshot's
// independent +1 references keep the keys alive for iteration. A callee-OWNED
// source (the RETURN site) frees its source RIGHT HERE — mirror #2299's
// `Set`/`Dictionary` arms — and a BORROWED source (the ARGUMENT site, the
// NAMED-source local-decl site) lets the source's binder reclaim at its own
// scope end. At the LOCAL-DECL site the free is additionally gated on the
// initializer being a `new` EXPRESSION, on the same alias-rejection gate the
// Set/Dictionary/LinkedList arms use — a producer-init IDENTIFIER source is
// still owned by that identifier, so freeing here would DOUBLE-FREE.
//
// ORDERING. `SortedSet` is ALWAYS hash-backed (`where T: IComparable<T>`
// excludes class elements, so the dense-array path is unreachable). The
// snapshot must be SORTED — stdlib §4.18 ("a comparison-ordered set …
// iteration visits elements in ascending order") — and `wrapSortedSetAsIEnumerable`
// already does the sort via `bz_sort_kv_gen`/`bz_sort_kv_str`. A fix that
// used `wrapArrayAsIEnumerable` over a SortedSet's `bz_dict_t*` reinterpreted
// as a `bz_array_t*` would pass the simple "iterate" rows while silently
// violating §4.18. Row (E) below pins that.
//
// ORACLE. `runAotChecked` compiles the generated program with AddressSanitizer
// + LeakSanitizer, so `r.ran` is simultaneously a no-fault, no-double-free and
// no-leak assertion. The leak row (G) is loop-amplified 200x because a
// one-shot LSan row can pass on both sides of a fix while the allocation is
// still reachable at exit.
//
// COVERAGE.
//   (A) The LOCAL-DECL face, PLAIN build — the issue's exact repro.
//   (B) The SAME program SANITIZED.
//   (C) The RETURN face — a method returning `IEnumerable<int>` from a
//       SortedSet, the third §29.2 wrap-grid face.
//   (D) The ARGUMENT face — passing a SortedSet as `IEnumerable<int>` and
//       calling `GetEnumerator()` inside the callee.
//   (E) ASCENDING-ORDER pin — the `wrapSortedSetAsIEnumerable` snapshot is
//       sorted; a fix that walked the dict in hash order would yield `27` (or
//       whatever) and this row would catch it.
//   (F) The DIRECT `.GetEnumerator()` surface — the control that already
//       worked (it routes through the same helper, but unconditionally).
//   (G) Unbound `new SortedSet` at a local-decl head, 200x under LSan —
//       the local-decl arm's `new`-gated `freeDictSourceNow`.
//   (H) IN-PLACE `for x in s do` — the control that already worked.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The issue's own repro, verbatim in shape. `s` is a named local, so the
// local-decl arm's `new`-gate declines the source free: `s` still owns the
// SortedSet and disposes it at its own scope end.
// (Never put a C++ `//` comment inside these literals: it is lexed as Bezel
// source and comments out the rest of the program. Bezel's own comment forms
// are `//`, `/* ... */` and `///` -- spec §1.5, which is exhaustive.)
const char* const kLocalDeclRepro =
    "module M; "
    "import System.Collections.Generic; "
    "public class Main "
    "  public static method Main(): int "
    "    using var s := new SortedSet<int>(); "
    "    s.Add(7); s.Add(4); "
    "    var en: IEnumerable<int> := s; "
    "    using var it := en.GetEnumerator(); "
    "    var acc: int := 0; "
    "    while it.MoveNext() do "
    "      acc := acc * 10 + it.Current; "
    "    end while; "
    "    Console.WriteLine($\"acc={acc}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

}  // namespace

// ---------------------------------------------------------------------------
// (A) The LOCAL-DECL face, PLAIN build.
//   per §13.4 (SortedSet), §5 (IEnumerable), §28.9 (boxed enumerator disposal)
//
// Pre-fix on e89658c: SIGSEGV, rc 139 — `en` held the raw `bz_dict_t*`,
// `en.GetEnumerator()` read slot 4 of that struct as a function pointer and
// called it. ASan reports `pc 0x0` (the dict's first word is 0).
// ---------------------------------------------------------------------------
BEZEL_TEST(sortedset_ienumerable_getenumerator_3695,
           sortedset_bound_to_ienumerable_local_iterates_ascending_13_4) {
    JitResult r = runAot(kLocalDeclRepro);
    // SortedSet<int> with 4 and 7 added => "47" (ascending).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "acc=47"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (B) The SAME program, SANITIZED — the wild call AND the disposal claim.
//   per §28.9 (boxed enumerator disposal), §28.2 (dispose exactly once)
// ---------------------------------------------------------------------------
BEZEL_TEST(sortedset_ienumerable_getenumerator_3695,
           sortedset_ienumerable_getenumerator_is_not_a_wild_call_28_9) {
    JitResult r = runAotChecked(kLocalDeclRepro);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "acc=47"));
}

// ---------------------------------------------------------------------------
// (C) The RETURN face — the third position `wrapAsIEnumerableForTarget` serves.
//   per §29.2 (Desugaring of `for`), §13.4, §5
//
// The SortedSet is the RETURN source, so `returnSourceIsCalleeOwnedCollection`
// classifies it as callee-owned and the coercion takes the owning arm. (A
// `using var` would be a sema error here: §28.10 forbids moving a
// scope-bound using var out of its scope.)
// ---------------------------------------------------------------------------
BEZEL_TEST(sortedset_ienumerable_getenumerator_3695,
           sortedset_return_as_ienumerable_iterates_29_2) {
    const std::string src =
        "module M; "
        "import System.Collections.Generic; "
        "public class Main "
        "  public static method Mk(): IEnumerable<int> "
        "    var s := new SortedSet<int>(); "
        "    s.Add(2); s.Add(1); s.Add(3); "
        "    return s; "
        "  end Mk; "
        "  public static method Main(): int "
        "    using var e := Mk(); "
        "    var n := 0; "
        "    for var x in e do n := n * 10 + x; end for; "
        "    Console.WriteLine($\"n={n}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=123"));
}

// ---------------------------------------------------------------------------
// (D) The ARGUMENT face — passing a SortedSet as `IEnumerable<int>`.
//   per §29.2, §5
// ---------------------------------------------------------------------------
BEZEL_TEST(sortedset_ienumerable_getenumerator_3695,
           sortedset_passed_as_ienumerable_argument_iterates_29_2) {
    const std::string src =
        "module M; "
        "import System.Collections.Generic; "
        "public class Main "
        "  public static method Sum(e: IEnumerable<int>): int "
        "    var n := 0; "
        "    for var x in e do n := n * 10 + x; end for; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var s := new SortedSet<int>(); "
        "    s.Add(5); s.Add(3); "
        "    Console.WriteLine($\"sum={Sum(s)}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "sum=35"));
}

// ---------------------------------------------------------------------------
// (E) ASCENDING-ORDER pin.
//   per stdlib §4.18 (SortedSet: iteration visits elements in ascending order)
//
// This is the row that separates the CORRECT fix from the tempting one. The
// keys are inserted 9, 1, 5; a hash-order walk would yield some other ordering
// (or the same numbers in a different sequence). A fix that used a plain
// `wrapArrayAsIEnumerable` over the raw `bz_dict_t*` reinterpreted as an
// array would pass rows (A)-(D) while silently violating §4.18 — a wrong
// answer with no fault, the silent shape. This row catches that.
// ---------------------------------------------------------------------------
BEZEL_TEST(sortedset_ienumerable_getenumerator_3695,
           sortedset_ienumerable_iteration_is_ascending_4_18) {
    const std::string src =
        "module M; "
        "import System.Collections.Generic; "
        "public class Main "
        "  public static method Main(): int "
        "    using var s := new SortedSet<int>(); "
        "    s.Add(9); s.Add(1); s.Add(5); "
        "    var en: IEnumerable<int> := s; "
        "    var acc: int := 0; "
        "    for var x in en do acc := acc * 10 + x; end for; "
        "    Console.WriteLine($\"acc={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "acc=159"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "acc=915"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "acc=591"));
}

// ---------------------------------------------------------------------------
// (F) The DIRECT `.GetEnumerator()` surface — the control that already worked.
//   per §13.4, §29.11
//
// Direct call on the concrete type routes through `wrapSortedSetAsIEnumerable`
// (`LlvmBackend_Calls2.cpp:14146`); this fix extends the §29.2 coercion +
// local-decl init to route through the SAME helper. Both surfaces should agree
// on ascending order; this row pins the direct surface that was already
// working so a future regression there also fails.
// ---------------------------------------------------------------------------
BEZEL_TEST(sortedset_ienumerable_getenumerator_3695,
           sortedset_direct_getenumerator_is_unchanged_29_11) {
    const std::string src =
        "module M; "
        "import System.Collections.Generic; "
        "public class Main "
        "  public static method Main(): int "
        "    using var s := new SortedSet<int>(); "
        "    s.Add(8); s.Add(2); s.Add(5); "
        "    using var it := s.GetEnumerator(); "
        "    var acc: int := 0; "
        "    while it.MoveNext() do acc := acc * 10 + it.Current; end while; "
        "    Console.WriteLine($\"acc={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "acc=258"));
}

// ---------------------------------------------------------------------------
// (G) Unbound `new SortedSet` at a local-decl head, 200x under LSan.
//   per §28.3, §29.2
//
// This is the local-decl arm's OWNING gate: a `new` at a local-decl site is
// bound to no other name by construction, so the coercion frees the source
// set itself (the snapshot is independent). Without that free the row leaks
// one `bz_dict_t` per iteration; with a free applied to the NAMED-source
// shape of row (A) instead, that row double-frees.
// ---------------------------------------------------------------------------
BEZEL_TEST(sortedset_ienumerable_getenumerator_3695,
           unbound_new_sortedset_at_ienumerable_local_is_freed_once_28_3) {
    const std::string src =
        "module M; "
        "import System.Collections.Generic; "
        "public class Main "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var en: IEnumerable<int> := new SortedSet<int>(); "
        "      for var x in en do total := total + x; end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    // The SortedSet is empty by construction, so the value observable is 0
    // and `r.ran` (no fault, no leak, no double free) carries the row.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=0"));
}

// ---------------------------------------------------------------------------
// (H) IN-PLACE `for x in s do` — the control that already worked.
//   per §13.4, §29.11
// ---------------------------------------------------------------------------
BEZEL_TEST(sortedset_ienumerable_getenumerator_3695,
           in_place_sortedset_iteration_is_unchanged_13_4) {
    const std::string src =
        "module M; "
        "import System.Collections.Generic; "
        "public class Main "
        "  public static method Main(): int "
        "    using var s := new SortedSet<int>(); "
        "    s.Add(6); s.Add(1); s.Add(4); "
        "    var acc: int := 0; "
        "    for var x in s do acc := acc * 10 + x; end for; "
        "    Console.WriteLine($\"acc={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "acc=146"));
}