// Ownership / leak — fix #6439 (§28.6 / §28.11, v0.338):
// an `owned IEnumerable<T>` FIELD store did not cascade to the collection's
// class elements. The elements leaked.
//
// THE DEFECT. A field declared `private owned _e: IEnumerable<Item>` stores an
// `OwningArray<Item>`. Because `IEnumerable<T>` is an iterator-format head
// (§29.7), the store does not keep the collection raw: it materialises an
// OWNING ENUMERATOR WRAPPER frame (`bz$arrenumown$ptr$Dispose`) holding the
// array pointer, and the holder's teardown calls that wrapper's slot-1 Dispose.
// That Dispose emitted `bz_array_free(self->arr, elDisp)` where `elDisp` came
// from `ownedWrapElementDisposer` → `elementPolicyFor(<T>, owning=false)`,
// which is NULL for a CLASS element by construction — the enumerator hands out
// borrows and so must not dispose them. The array was freed; the `Item`s were
// stranded. MEASURED on base: `32 byte(s) leaked in 2 allocation(s)`, both
// frames `bz$M.Holder$ctor`.
//
// The array's own HEADER already carried the right answer: #2608 stamps
// `elem_disposer` on every owning `bz_array_t` at construction (`StampSite 1`
// and the array-literal site, both in `LlvmBackend_Setup.cpp`), and the IR for
// the repro shows `bz$elemDispose` emitted and the wrap calling
// `bz_array_free(ptr %2, ptr null)`. §28.6's cascade and #2608's tag were both
// already in place; the free site simply never asked.
//
// INDEPENDENT OF THE GETTER. Row (a) never reads the property — the field is
// stored on construction and the program exits — so this is a store-time
// cascade gap, not a read-time or iterator-registration gap. That is what makes
// it worth pinning separately from the getter family.
//
// WHY THE HEAD IS THE DISCRIMINATOR (the load-bearing part of this fix).
// Consulting the header is NOT unconditionally sound, and the obvious
// header-always variant is an over-dispose — measured, not assumed:
//
//   * `IEnumerable<T>` head — the wrap installs the BORROWING `Current`
//     (#2710's rule). Iteration LENDS each element; the collection still holds
//     every one of them at Dispose, so the header is authoritative. Rows
//     (a)-(d) pin this.
//   * `IOwningEnumerable<T>` head — §29.1/§29.2 make slot 3 `TakeCurrent`, so
//     ownership LEAVES the collection at each yield. `wrapArrayAsIEnumerable`
//     routes that head to the draining wrap (`owningDrainWrapForHead`), whose
//     `bz_array_drain_next` REMOVES each slot, so the header is a harmless
//     no-op on an emptied array. But the OWNING twins
//     (`wrapOwnedArrayAsIEnumerable` / `wrapOwnedStackAsIEnumerator`) do NOT
//     drain: MEASURED, zero `bz_array_drain_next` calls in the IR of a
//     callee-owned `IOwningEnumerable<Item>` return. There the loop has already
//     discharged each element while the array still holds the pointer, and
//     consulting the header is a DOUBLE DISPOSE — `heap-use-after-free`,
//     READ of size 8 in `bz$elemDispose` <- `bz_array_free`, reproduced on the
//     header-always variant, which turned `iowningenumerable_getter_value_arm_3355`
//     and `callee_owned_stack_return_wrap_3450` red.
//
// Rows (e)-(f) are the NEGATIVE CONTROL that makes the fix falsifiable: they
// are the two owning-head shapes that a header-always rule breaks, pinned on
// `runAotChecked` so a reintroduced over-dispose fails HERE rather than
// surfacing later as a sanitizer report in some other lane's test.
//
// ORACLE. Every row is on `runAotChecked`, which always runs the program under
// ASan/LSan. `leaksAtLeast` is NOT used: it is a leak oracle, not an ASan
// oracle, and a row that pins it can go green while the sanitizer is not
// running at all. Each row asserts `r.ran` AND the exact `exitCode` AND — for
// the rows that compute — the printed value, so a wrong answer cannot hide
// behind a leak that happens to be fixed. a toolchain SKIP (an absent ASan link) FAILS
// loudly via `checkedToolchainSkipped`, it is never absorbed.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string stdoutOf(const JitResult& r) { return r.stdoutText; }

}  // namespace

// ---------------------------------------------------------------------------
// (a) THE REPRO. An `owned IEnumerable<Item>` field, stored and never read.
//     Base: `32 byte(s) leaked in 2 allocation(s)`.
// ---------------------------------------------------------------------------
BEZEL_TEST(owned_enumerable_field_cascade_6439,
           field_store_no_read_no_leak_28_6) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _e: IEnumerable<Item>; "
        "  public constructor() "
        "    var a := new OwningArray<Item>(); "
        "    a.Add(new Item(3)); "
        "    a.Add(new Item(4)); "
        "    _e := a; "
        "  end constructor; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // LSan reports the leak as ran + nonzero exit
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (b) The stack twin: `OwningStack<Item>` behind the same field type.
//     Both array-backed owning leaves changed (the stack wrap's cache key too),
//     so this pins the twin rather than trusting it to follow. §28.11.
// ---------------------------------------------------------------------------
BEZEL_TEST(owned_enumerable_field_cascade_6439,
           field_store_stack_no_leak_28_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _e: IEnumerable<Item>; "
        "  public constructor() "
        "    var s := new OwningStack<Item>(); "
        "    s.Push(new Item(3)); "
        "    s.Push(new Item(4)); "
        "    _e := s; "
        "  end constructor; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (c) THE AMPLIFIED ROW, and the anti-vacuity guard. 300 holders x 300 items
//     is a one-shot leak whose signal would be a single ASan report either
//     way; the sum is printed and asserted so a row that "passes" by printing
//     the wrong number, or by never running, cannot go green. Base:
//     `1439968 byte(s) leaked in 89998 allocation(s)`, printing `13545000`.
// ---------------------------------------------------------------------------
BEZEL_TEST(owned_enumerable_field_cascade_6439,
           field_read_amplified_sums_correctly_28_6) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _e: IEnumerable<Item>; "
        "  public constructor() "
        "    var a := new OwningArray<Item>(); "
        "    for var i in 1..300 do a.Add(new Item(i)); end for; "
        "    _e := a; "
        "  end constructor; "
        "  public property E: IEnumerable<Item> => _e; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..300 do "
        "      using var h := new Holder(); "
        "      for var x in h.E do total := total + x.V; end for; "
        "    end for; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 300 holders x (1+2+...+300) = 300 x 45150 = 13545000. Asserted on
    // STDOUT, not on the exit code: exit codes are truncated to 8 bits, so an
    // exit-code assertion cannot distinguish this from any other value.
    BEZEL_EXPECT_TRUE(stdoutOf(r).find("13545000") != std::string::npos);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (d) A `string` element behind the same head. The header carries NO disposer
//     for a borrowing-family string array, so this row pins that the fix FALLS
//     BACK to the element-type-derived release rather than skipping it. §28.9 /
//     #2710 — the +1 retained at insertion must still be released.
// ---------------------------------------------------------------------------
BEZEL_TEST(owned_enumerable_field_cascade_6439,
           field_store_string_element_releases_28_9) {
    const std::string src =
        "module M; "
        "class Holder "
        "  private owned _e: IEnumerable<string>; "
        "  public constructor() "
        "    var a := new Array<string>(); "
        "    a.Add(\"aa\"); "
        "    a.Add(\"bb\"); "
        "    _e := a; "
        "  end constructor; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// NEGATIVE CONTROL — the over-dispose direction.
//
// These two rows are the reason the fix is head-gated rather than
// header-always. Both are the OWNING `IOwningEnumerable<Item>` head, where
// §29.1's `TakeCurrent` has already moved ownership out to the loop and the
// non-draining owning wrap's array still names the freed pointers. A
// header-always rule turns BOTH of these into a heap-use-after-free inside
// `bz$elemDispose` <- `bz_array_free` (MEASURED on that variant).
//
// They are pinned here, not merely observed, so that reintroducing the
// over-dispose fails in THIS file with a named test rather than surfacing as
// an unexplained sanitizer report somewhere else.
// ===========================================================================

// ---------------------------------------------------------------------------
// (e) Stored-view face: `IOwningEnumerable<Item>` property over an owned
//     `OwningArray<Item>` field, iterated with an `owned` loop variable. The
//     loop discharges each element; the wrap must not discharge it again.
//     Expected sum 3+4 = 7.
// ---------------------------------------------------------------------------
BEZEL_TEST(owned_enumerable_field_cascade_6439,
           owning_head_stored_view_no_double_dispose_28_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _a: OwningArray<Item>; "
        "  public constructor() "
        "    _a := new OwningArray<Item>(); "
        "    _a.Add(new Item(3)); "
        "    _a.Add(new Item(4)); "
        "  end constructor; "
        "  public property Items: IOwningEnumerable<Item> "
        "    get => do return _a; end; "
        "  end Items; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    var total := 0; "
        "    using var h := new Holder(); "
        "    for var x in h.Items do "
        "      using var y := owned x; "
        "      total := total + y.V; "
        "    end for; "
        "    return total; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);  // 3 + 4, and no double dispose
}

// ---------------------------------------------------------------------------
// (f) RETURN face: a callee-owned array returned at an `IOwningEnumerable<Item>`
//     head, which routes through `wrapOwnedArrayAsIEnumerable` — the very site
//     the fix edits, on the head where consulting the header is WRONG. This is
//     the sibling of `callee_owned_stack_return_wrap_3450`'s own IOE cell, kept
//     here so this file's negative control does not depend on another file's
//     row staying in place. Expected sum 3+4 = 7.
// ---------------------------------------------------------------------------
BEZEL_TEST(owned_enumerable_field_cascade_6439,
           owning_head_callee_owned_return_no_double_dispose_28_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class G "
        "  public static method Make(): IOwningEnumerable<Item> "
        "    var a := new OwningArray<Item>(); "
        "    a.Add(new Item(3)); "
        "    a.Add(new Item(4)); "
        "    return a; "
        "  end Make; "
        "end G; "
        "class P "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var x in G.Make() do "
        "      using var y := owned x; "
        "      total := total + y.V; "
        "    end for; "
        "    return total; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);  // 3 + 4, and no double dispose
}