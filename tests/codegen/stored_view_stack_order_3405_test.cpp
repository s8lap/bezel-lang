// Codegen / iteration order — fix #3405 (stdlib §4.11 / §4.13; spec §29.1 /
// §29.2 / §23.2, v0.283): a `Stack<T>` / `OwningStack<T>` read through a
// §28.6 STORED-VIEW property face iterated in forward index order, where
// stdlib §4.11 requires pop order.
//
// THE AUTHORITY. stdlib §4.11 `Stack<T>`, `OwningStack<T>`, `Queue<T>`, and
// `OwningQueue<T>`, "Iteration order (v0.195)" — quoted verbatim (v0.283):
//
//   "All four types iterate (via `IReadOnlyCollection<T>`) in take order — a
//    stack top-to-bottom (pop order), a queue front-to-back (dequeue order);
//    the owning forms iterate identically, yielding borrows."
//
// THE DEFECT — a duplicated classification predicate, #3419's pattern.
// `wrapAsIEnumerableForTarget` (the wrap-creator every argument, local-decl,
// and RETURN coercion routes through) already knows the stack family needs
// the reverse machinery: #2471 built it, #2472 and #2486 wired the for-loop,
// the explicit `.GetEnumerator()`, and the three coercion positions to it, and
// `stack_ienumerable_coercion_order_2486_test.cpp` pins "all five coercion
// paths agree". `wrapPropertyArrayAsIEnumerable` — the SIXTH face, the
// expression-bodied / bare-identifier property read — carried a SECOND
// spelling of the same array-family grid and routed the whole family straight
// to the FORWARD `wrapArrayAsIEnumerable`. The sixth face was in no test, so
// the divergence survived every one of those fixes.
//
// MEASURED ON 1e3861a (the parent of this commit), same three elements pushed
// 5, 10, 20 (bottom to top), the observable being `o := o * 100 + v`:
//
//   face                                    pre-fix    §4.11 / post-fix
//   ------------------------------------    -------    ----------------
//   `for x in c.Borrowing` (property)        51020          201005
//   `Walk(c.Raw)`          (argument)       201005          201005
//   `for x in c.Raw`       (direct)         201005          201005
//
// At the `IOwningEnumerable<Item>` head the same read additionally DRAINED the
// source front-to-back — #3405 as filed. #3385 made that walk destructive (it
// is supposed to be: §29.2 desugars an owning-typed source through
// `TakeCurrent()`), so before this fix a caller got the wrong order *and* a
// drained collection.
//
// THE FIX. The array-family arm at the property face is DELETED rather than
// aligned, per #3419 point 2 ("delete the other spellings — an aligned copy
// drifts again on the next family member"): every family now dispatches
// through `wrapAsIEnumerableForTarget`, so the property face has ZERO
// spellings of the grid and inherits the stack divert, the §12.9 nested
// covariant view, and the #2299 ownership arm by construction.
//
// Deleting it exposed a second half. At the owning head the authority routes
// the stack family to `wrapStackAsIEnumerator`, which #3385 never taught the
// owning-head divert it gave the forward leaf — so the delete alone would have
// traded FIFO-and-draining for LIFO-and-BORROWING, which is #3385's own
// double-dispose (the loop discharges each element per §29.2 while the field's
// `OwningStack` still holds it). Both leaves now ask ONE function,
// `owningDrainWrapForHead`, and `wrapArrayAsIOwningEnumerator` takes a `lifo`
// key selecting the `bz_array_drain_next_lifo` primitive that already existed
// for `for var x in stk.Drain()`. Order and destructiveness are therefore
// asserted TOGETHER in every row below: the fix must change the order without
// changing whether the source drains.
//
// COVERAGE.
//   (A) The owning head — pop order AND the drain survives (Remaining == 0).
//   (B) The borrowing head — pop order AND the read stays NON-destructive
//       (Remaining == 3). The twin the issue did not name; same routing.
//   (C) Three faces in one program: property read, argument coercion, direct
//       for-in. This is the anti-drift assertion — #2486's "five paths agree"
//       test could not see the sixth.
//   (D) Verified negatives at the same site: `Queue` and `LinkedList` keep
//       front-to-back / head-to-tail. The routing change must not touch them.
//   (E) The §12.9 nested covariant view at the property face — SIGSEGV on
//       1e3861a, because the deleted copy never consulted
//       `nestedIEnumViewNeeded`.
//   (F) Partial drain at the owning head (stdlib §4.5): `break` after one
//       element takes the TOP and leaves the remainder in the field, disposed
//       exactly once there. Under ASan/LSan.
//   (G) `OwningQueue` at the owning head still drains front-to-back — the
//       verified negative for the ROUTING decision: which leaf passes which
//       `lifo` value. MEASURED: rebuilding with the forward leaf's call
//       changed to `owningDrainWrapForHead(arr, iEnumTy, true)` makes this
//       row print `order=201005` and go red.
//   (H) The `lifo` CACHE-KEY guard, which (G) does NOT provide. The two
//       variants share a struct layout and differ only by the `lifo$` segment
//       of the per-element-type name prefix, so a key-less build silently
//       gives one family the other's vtable. Only a program holding BOTH an
//       `OwningStack<Item>` and an `OwningArray<Item>` at `IOwningEnumerable`
//       heads can see it — a single-family program instantiates one variant
//       and is blind. MEASURED on a scratch compiler with the `lifo$` segment
//       removed: this row prints `stack=201005 array=201005` and goes red,
//       while (G), the whole of this file's other six rows, and the
//       `iowningenumerable` / `drain` / `owning_queue` filters all stay green.
//   (I) The EXPRESSION-BODIED INDEXER getter (§23.2), the third read site
//       `wrapPropertyArrayAsIEnumerable` serves besides the qualified and
//       bare-`this` property reads (`LlvmBackend_Assign.cpp:2301`). Same
//       routing, same defect: 51020 on 1e3861a, 201005 with the fix.

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

// The §28.6 stored-view face over an owning-stack field, exposed at both
// enumerable heads and (for the cross-face row) at its own concrete type.
// Pushed 5, 10, 20 — bottom to top per stdlib §4.11's literal convention, so
// pop order is 20, 10, 5.
const char* const kPrelude =
    "module M; "
    "class Item "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item; "
    "class C "
    "  private owned _s: OwningStack<Item>; "
    "  public constructor() "
    "    _s := new OwningStack<Item>(); "
    "    _s.Push(new Item(5)); "
    "    _s.Push(new Item(10)); "
    "    _s.Push(new Item(20)); "
    "  end constructor; "
    "  public property Owning: IOwningEnumerable<Item> => _s; "
    "  public property Borrowing: IEnumerable<Item> => _s; "
    "  public property Raw: OwningStack<Item> => _s; "
    "  public property Remaining: int => _s.Count; "
    "end C; ";

}  // namespace

// ---------------------------------------------------------------------------
// (A) The #3405 pin — `IOwningEnumerable<Item>` stored view.
//   per stdlib §4.11 "Iteration order" (pop order), stdlib §4.5 / §4.13
//   (elements leave the collection as they are yielded), spec §29.1
//   (TakeCurrent), §29.2 (owning desugaring), §28.6 (the stored-view face)
// ---------------------------------------------------------------------------
BEZEL_TEST(stored_view_stack_order_3405,
           owning_head_stored_view_drains_in_pop_order_4_11) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var order := 0; "
        "    for var x in c.Owning do "
        "      using var y := owned x; "
        "      order := order * 100 + y.V; "
        "    end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return c.Remaining; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on 1e3861a: "order=51020" — forward index order, the FIFO drain
    // #3405 reports. Post-fix 20, 10, 5 = stdlib §4.11's pop order.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=201005"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "order=51020"));
    // …and the walk is still DESTRUCTIVE at this head (#3385). Changing the
    // order must not change whether the source drains.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (B) The borrowing-head twin — same field, `IEnumerable<Item>` head.
//   per stdlib §4.11 ("the owning forms iterate identically, yielding
//   borrows"), spec §29.1 (`Current` is non-destructive), §28.6
// ---------------------------------------------------------------------------
BEZEL_TEST(stored_view_stack_order_3405,
           borrowing_head_stored_view_pop_order_and_non_destructive_4_11) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var order := 0; "
        "    for var x in c.Borrowing do "
        "      order := order * 100 + x.V; "
        "    end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return c.Remaining; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on 1e3861a: "order=51020". The issue named only the owning head;
    // the routing defect was head-independent.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=201005"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "order=51020"));
    // §29.1's borrowing `Current` lends; all three elements are still there.
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// (C) The anti-drift row: the property face, the argument coercion, and the
//     direct for-in over the SAME stack must agree.
//   per stdlib §4.11; #2486's "all five coercion paths agree" extended to the
//   sixth face (the §23.2 property read) it never covered
// ---------------------------------------------------------------------------
BEZEL_TEST(stored_view_stack_order_3405,
           property_face_agrees_with_argument_and_direct_faces_4_11) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Walk(e: IEnumerable<Item>): int "
        "    var o := 0; "
        "    for var x in e do "
        "      o := o * 100 + x.V; "
        "    end for; "
        "    return o; "
        "  end Walk; "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var viaProperty := 0; "
        "    for var a in c.Borrowing do "
        "      viaProperty := viaProperty * 100 + a.V; "
        "    end for; "
        "    var viaArg := Walk(c.Raw); "
        "    var viaDirect := 0; "
        "    for var d in c.Raw do "
        "      viaDirect := viaDirect * 100 + d.V; "
        "    end for; "
        "    Console.WriteLine("
        "      $\"prop={viaProperty} arg={viaArg} direct={viaDirect}\"); "
        "    return c.Remaining; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on 1e3861a: "prop=51020 arg=201005 direct=201005" — the two
    // authorities that already agreed, and the copy that had drifted.
    BEZEL_EXPECT_TRUE(has(r.stdoutText,
                          "prop=201005 arg=201005 direct=201005"));
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// (D) Verified negatives at the same site — the siblings the routing change
//     must NOT disturb.
//   per stdlib §4.11 ("a queue front-to-back (dequeue order)") for the queue
//   half. The LinkedList half is a MEASURED invariance claim, not a cited
//   one: stdlib §4.6 `LinkedList<T>` states head-to-tail order only for
//   SEQUENCE-LITERAL construction ("textual order is head-to-tail:
//   `using var l: LinkedList<int> := [10, 20, 30];` produces a list whose
//   `First` is `10` and `Last` is `30`"), and carries no iteration-order
//   sentence of its own. What this row pins is that the value is IDENTICAL
//   on 1e3861a and with the fix — the LinkedList family never went through
//   the deleted arm, so the routing change must not perturb it.
// ---------------------------------------------------------------------------
BEZEL_TEST(stored_view_stack_order_3405,
           queue_and_linkedlist_stored_views_keep_forward_order_4_11) {
    const std::string src =
        "module M; "
        "class C "
        "  private owned _q: Queue<int>; "
        "  private owned _l: LinkedList<int>; "
        "  public constructor() "
        "    _q := new Queue<int>(); "
        "    _q.Enqueue(5); "
        "    _q.Enqueue(10); "
        "    _q.Enqueue(20); "
        "    _l := new LinkedList<int>(); "
        "    _l.AddLast(5); "
        "    _l.AddLast(10); "
        "    _l.AddLast(20); "
        "  end constructor; "
        "  public property Q: IEnumerable<int> => _q; "
        "  public property L: IEnumerable<int> => _l; "
        "end C; "
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var q := 0; "
        "    for var x in c.Q do "
        "      q := q * 100 + x; "
        "    end for; "
        "    var l := 0; "
        "    for var y in c.L do "
        "      l := l * 100 + y; "
        "    end for; "
        "    Console.WriteLine($\"queue={q} llist={l}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Identical before and after the fix — measured on 1e3861a. A queue's
    // physical forward order IS its dequeue order, and the LinkedList family
    // never went through the deleted arm at all.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "queue=51020 llist=51020"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (E) The §12.9 nested covariant view at the property face.
//   per spec §12.9 / §29.1 (#1850, #2487): an element that is itself an
//   array-backed collection viewed as `IEnumerable<T>` needs the per-element
//   re-wrapping enumerator, which only the wrap-creator knows about
// ---------------------------------------------------------------------------
BEZEL_TEST(stored_view_stack_order_3405,
           nested_covariant_view_at_property_face_no_longer_faults_12_9) {
    const std::string src =
        "module M; "
        "class C "
        "  private owned _r: Array<int>; "
        "  private owned _rows: Array<Array<int>>; "
        "  public constructor() "
        "    _r := new Array<int>(); "
        "    _r.Add(1); "
        "    _r.Add(2); "
        "    _rows := new Array<Array<int>>(); "
        "    _rows.Add(_r); "
        "  end constructor; "
        "  public property Rows: IEnumerable<IEnumerable<int>> => _rows; "
        "end C; "
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var n := 0; "
        "    for var row in c.Rows do "
        "      for var x in row do "
        "        n := n * 10 + x; "
        "      end for; "
        "    end for; "
        "    Console.WriteLine($\"n={n}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAot(src);
    // Pre-fix on 1e3861a: SIGSEGV (exit 139) — the deleted copy handed the
    // inner raw `bz_array_t*` to a consumer that drove vtable dispatch on it.
    // The identical shape at an ARGUMENT coercion returned 12 on the same
    // build, which is the divergence.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=12"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (F) Partial drain at the owning head — the TOP leaves, the rest stays.
//   per stdlib §4.5 ("partial iteration leaves the remainder in place"),
//   §4.11 (pop order), spec §28.2 (dispose exactly once), §28.3
// ---------------------------------------------------------------------------
BEZEL_TEST(stored_view_stack_order_3405,
           partial_drain_takes_the_top_and_leaves_the_rest_4_5) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var order := 0; "
        "    for var x in c.Owning do "
        "      using var y := owned x; "
        "      order := order * 100 + y.V; "
        "      break; "
        "    end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return c.Remaining; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    // ASan/LSan clean via r.ran: the one element that left is discharged by
    // the loop and NOT re-disposed by the field's element-disposer pass, and
    // the two that stayed are disposed there exactly once.
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on 1e3861a: "order=5" — the BOTTOM element. Post-fix the TOP.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=20"));
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// ---------------------------------------------------------------------------
// (G) The ROUTING negative: `OwningQueue` at the owning head still drains
//     front-to-back, i.e. the forward leaf still passes `lifo=false`.
//
// ⚠ This row is NOT the guard for the `lifo` CACHE KEY, which an earlier
// revision of this header claimed it was. Measured: a scratch compiler with
// the `lifo$` segment removed from the name prefix leaves this row GREEN —
// the program instantiates only the queue's variant, so there is nothing for
// it to collide with. Row (H) is the cache-key guard. What this row DOES
// prove, also measured: rebuilding with the forward leaf's call changed to
// `owningDrainWrapForHead(arr, iEnumTy, /*lifo=*/true)` makes it print
// `order=201005` and fail.
//   per stdlib §4.11 ("a queue front-to-back (dequeue order)"), §4.13
// ---------------------------------------------------------------------------
BEZEL_TEST(stored_view_stack_order_3405,
           owning_queue_head_still_drains_front_to_back_4_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class C "
        "  private owned _q: OwningQueue<Item>; "
        "  public constructor() "
        "    _q := new OwningQueue<Item>(); "
        "    _q.Enqueue(new Item(5)); "
        "    _q.Enqueue(new Item(10)); "
        "    _q.Enqueue(new Item(20)); "
        "  end constructor; "
        "  public property Owning: IOwningEnumerable<Item> => _q; "
        "  public property Remaining: int => _q.Count; "
        "end C; "
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var order := 0; "
        "    for var x in c.Owning do "
        "      using var y := owned x; "
        "      order := order * 100 + y.V; "
        "    end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return c.Remaining; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // Unchanged by this fix — measured identical on 1e3861a and after.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=51020"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (H) The `lifo` CACHE-KEY guard — the one shape that can observe it.
//
// `wrapArrayAsIOwningEnumerator` caches its Dispose/MoveNext/TakeCurrent/
// GetOwningEnumerator functions and its vtable under a per-element-type name
// prefix. The LIFO and front-to-back variants have the SAME struct layout and
// the SAME element type, so without the `lifo$` segment in that prefix the
// SECOND family to be lowered silently reuses the FIRST family's vtable —
// including its drain primitive.
//
// A single-family program cannot see this: it instantiates one variant and
// there is nothing to collide with. Row (G) is exactly such a program, which
// is why it stays green on a key-less build. This row holds an
// `OwningStack<Item>` AND an `OwningArray<Item>` at `IOwningEnumerable<Item>`
// heads in one compilation unit, so both variants are minted for element type
// `ptr` and a shared key makes them the same object.
//
// MEASURED on a scratch compiler built with the `lifo$` segment removed:
//   stack=201005 array=201005   <- the array inherited the stack's LIFO vtable
// and with the key in place:
//   stack=201005 array=51020
// Both builds drain both collections (srem=0 arem=0), so the failure mode is
// a SILENT wrong order, never a crash or a leak — nothing but an
// order assertion over two families in one program can catch it.
//   per stdlib §4.11 "Iteration order" (both halves of the sentence at once),
//   §4.5 / §4.13
// ---------------------------------------------------------------------------
BEZEL_TEST(stored_view_stack_order_3405,
           lifo_and_forward_owning_drains_do_not_share_a_vtable_4_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class C "
        "  private owned _s: OwningStack<Item>; "
        "  private owned _a: OwningArray<Item>; "
        "  public constructor() "
        "    _s := new OwningStack<Item>(); "
        "    _s.Push(new Item(5)); "
        "    _s.Push(new Item(10)); "
        "    _s.Push(new Item(20)); "
        "    _a := new OwningArray<Item>(); "
        "    _a.Add(new Item(5)); "
        "    _a.Add(new Item(10)); "
        "    _a.Add(new Item(20)); "
        "  end constructor; "
        "  public property S: IOwningEnumerable<Item> => _s; "
        "  public property A: IOwningEnumerable<Item> => _a; "
        "  public property SRem: int => _s.Count; "
        "  public property ARem: int => _a.Count; "
        "end C; "
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var s := 0; "
        "    for var x in c.S do "
        "      using var y := owned x; "
        "      s := s * 100 + y.V; "
        "    end for; "
        "    var a := 0; "
        "    for var p in c.A do "
        "      using var q := owned p; "
        "      a := a * 100 + q.V; "
        "    end for; "
        "    Console.WriteLine("
        "      $\"stack={s} array={a} srem={c.SRem} arem={c.ARem}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // The two families must disagree — §4.11 gives the stack pop order and the
    // array (an `OwningArray`, no §4.11 reversal) front-to-back. A key-less
    // build prints "array=201005" here.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "stack=201005 array=51020"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "array=201005"));
    // Both still drain — the key affects the ORDER, never the destructiveness.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "srem=0 arem=0"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (I) The expression-bodied INDEXER getter — the third read site
//     `wrapPropertyArrayAsIEnumerable` serves.
//
// The helper is applied at the qualified property read and the bare-`this`
// read (`LlvmBackend_Exprs.cpp`) and at the expression-bodied indexer getter
// (`LlvmBackend_Assign.cpp:2301`). Rows (A)-(C) cover the property spelling;
// the indexer took the identical deleted arm and had the identical defect.
// MEASURED: 51020 on 1e3861a, 201005 with the fix.
//   per stdlib §4.11 "Iteration order", spec §23.2 (expression-bodied
//   accessors), §29.2
// ---------------------------------------------------------------------------
BEZEL_TEST(stored_view_stack_order_3405,
           expression_bodied_indexer_getter_also_pops_in_order_4_11) {
    const std::string src =
        std::string(kPrelude) +
        "class D "
        "  private owned _s: OwningStack<Item>; "
        "  public constructor() "
        "    _s := new OwningStack<Item>(); "
        "    _s.Push(new Item(5)); "
        "    _s.Push(new Item(10)); "
        "    _s.Push(new Item(20)); "
        "  end constructor; "
        "  public property [i: int]: IEnumerable<Item> => _s; "
        "end D; "
        "class Main "
        "  public static method Run(): int "
        "    using var d := new D(); "
        "    var order := 0; "
        "    for var x in d[0] do "
        "      order := order * 100 + x.V; "
        "    end for; "
        "    Console.WriteLine($\"indexer={order}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on 1e3861a: "indexer=51020".
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "indexer=201005"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "indexer=51020"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
