// fix #4749 (stdlib §4.5, §4.13, §5 + spec §26.5 rule 1, v0.307): the §5
// LINQ `ToArray()` extension is reachable on an `OwningArray<T>` receiver —
// the type implements `IEnumerable<T>` (§4.13) and declares no instance
// member of that name, so per spec §26.5 rule 1 "instance members win — by
// name … extension blocks are never consulted" does NOT commit instance
// resolution for `ToArray` on this receiver, and the extension is consulted.
// It returns an `owned Array<T>` — a **borrowing snapshot**: at a class
// element type `Array<T>` is a borrowing collection, so its slots hold
// borrows of the elements the source still owns. The pre-fix sema reject
// at `src/sema/ExprTypecheck_Calls.cpp` ~6399 (the
// "`OwningArray<T>` does not expose `ToArray` — an owning collection has
// no non-owning copy-form (copying would double-own its elements, §4.5)"
// diagnostic) predated the #3286 ruling and contradicted §4.5's explicit
// borrowing-snapshot paragraph.
//
// Rows:
//   R1  the issue's own repro — `OwningArray<T>.ToArray()` on the exact
//       §4.5 sample shape (one element, snapshot.Count == 1).  Value
//       assertion: stdout "1\n" byte-for-byte.  Pre-fix the row failed at
//       compile time on the rejection diagnostic above; post-fix it
//       compiles AND runs to the expected snapshot count.
//   R2  OwningArray<T>.ToSet() — also a §4.5-generalized §5 LINQ
//       extension ("The same reachability and the same obligation hold for
//       ToSet(), ToDictionary(…), and every other §5 operator applied to
//       an owning receiver").  Pre-fix rejected; post-fix accepts and runs
//       (one element → snapshot.Count == 1).  Pins that the fix is not
//       a one-name carve-out.
//   R3  OwningDictionary<K,V>.First / Last / Single / ElementAt — the
//       sibling reject at ~8099 is a SEPARATE block (its own §4.9
//       conformance citation) and MUST still fire.  Confirms blast
//       radius: the fix to the ToArray/ToSet/ToDictionary block did NOT
//       touch the First/Last/Single/ElementAt reject on
//       OwningDictionary<K,V>.
//
// The OwningDictionary-Keystrokes case is the canonical
// reject-stays-pinned control: it is the row named in the "Care required"
// of the issue body, and the OwningDictionary<X,Y> shape — paired-key dict,
// not class-element-set — is the exact same `colName == "OwningDictionary"`
// site the OwningArray fix is adjacent to, so a sloppy fix that touched
// the broader gate would surface here.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── R1 — the issue's own repro. One element added; ToArray() returns a
// borrowing snapshot; snapshot.Count reads 1 (§4.5).
BEZEL_TEST(sema_owningarray_toarray_4749,
           owning_array_toarray_borrowing_snapshot_accepted_4_5_5) {
    auto r = runAot(
        "module M4749; "
        "public class Widget "
        "  public constructor() end constructor; "
        "end Widget; "
        "public class Program "
        "  public static method Main() "
        "    using var pool := new OwningArray<Widget>(); "
        "    pool.Add(new Widget()); "
        "    using var snapshot := pool.ToArray(); "
        "    Console.WriteLine(snapshot.Count); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// ── R2 — OwningArray<T>.ToSet() is also reachable per §4.5's
// generalization ("The same reachability and the same obligation hold for
// ToSet(), ToDictionary(…), and every other §5 operator applied to an
// owning receiver").  Same one-element shape; Set<T>.Count == 1.
BEZEL_TEST(sema_owningarray_toarray_4749,
           owning_array_toset_borrowing_snapshot_accepted_4_5_5) {
    auto r = runAot(
        "module M4749B; "
        "public class Widget "
        "  public constructor() end constructor; "
        "end Widget; "
        "public class Program "
        "  public static method Main() "
        "    using var pool := new OwningArray<Widget>(); "
        "    pool.Add(new Widget()); "
        "    using var snapshot := pool.ToSet(); "
        "    Console.WriteLine(snapshot.Count); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// ── R3 — OwningDictionary<K,V>.First() still rejected.  Per stdlib §4.9,
// `OwningDictionary<K, V>`'s conformance is `IDisposable` only — no
// `ICollection<DictionaryEntry<K, V>>` entry view — so `First`/`Last`/
// `Single`/`ElementAt` are not part of the stdlib surface at all on this
// type, and a sibling reject at `ExprTypecheck_Calls.cpp` ~8099 cites
// §4.9's conformance sentence by name.  The fix to the §4.5
// ToArray/ToSet/ToDictionary block must not touch this.
BEZEL_TEST(sema_owningarray_toarray_4749,
           owning_dictionary_first_still_rejected_blast_radius_4_9) {
    auto r = runAot(
        "module M4749C; "
        "public class V "
        "  public constructor() end constructor; "
        "end V; "
        "public class Program "
        "  public static method Main() "
        "    using var od := new OwningDictionary<int, V>(); "
        "    od.Add(1, new V()); "
        "    var fe := od.First(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`OwningDictionary<K, V>` does not expose `First`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§4.9"));
}