// fix #4750 / #4772, pin #4771 (codegen; spec §29.1 / §29.2; stdlib §4.5 /
// §4.7 / §4.10 / §4.11 / §4.13, v0.307): a builtin drain view bound to a NAMED
// local — `using var view := pool.Drain();` — used to reach `lowerLocalDecl`
// with no value (the member call itself lowers to nothing; only the for-in and
// GetOwningEnumerator dispatch recognise the shape), so the slot was degraded
// to zero behind the #2434 note:
//
//   internal: lowerLocalDecl's initializer expression failed to lower to a
//   value for this local's slot (degraded to a zero-initialized value) — #2434
//
// stdlib §4.13: "every owning collection's `Drain()` mints a fresh drain-view
// frame — a caller-owned enumerable over the collection". The fix mints it, so
// nothing degrades and nothing may say it did. MEASURED on the pre-fix tree
// (r20-c25): every one of the five heads below printed the note.
//
// THIS FILE IS THE DIAGNOSTIC ORACLE. It has to be lit rather than gtest: the
// AOT gtest harness populates `JitResult::diagnostics` only on a compile
// REJECT, so an "assert no #2434" there passes VACUOUSLY (proved on the pre-fix
// build). The runtime half — the right element, IOE, exactly-once disposal,
// no leak — is pinned in tests/codegen/ioe_slot_init_4750_test.cpp and
// tests/codegen/ioe_takecurrent_ioe_4750_test.cpp.
//
// One method per head of the drain family (OwningArray, OwningQueue,
// OwningSet, OwningStack, OwningLinkedList); OwningDictionary's drain yields
// `DrainedEntry<K, V>` and is out of scope.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class W
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end W;

class C
  public static method A(): int
    using var pool := new OwningArray<W>();
    pool.Add(new W(1));
    using var view := pool.Drain();
    using var it := view.GetOwningEnumerator();
    return pool.Count;
  end A;
  public static method Q(): int
    using var pool := new OwningQueue<W>();
    pool.Enqueue(new W(1));
    using var view := pool.Drain();
    using var it := view.GetOwningEnumerator();
    return pool.Count;
  end Q;
  public static method S(): int
    using var pool := new OwningSet<W>();
    pool.Add(new W(1));
    using var view := pool.Drain();
    using var it := view.GetOwningEnumerator();
    return pool.Count;
  end S;
  public static method K(): int
    using var pool := new OwningStack<W>();
    pool.Push(new W(1));
    using var view := pool.Drain();
    using var it := view.GetOwningEnumerator();
    return pool.Count;
  end K;
  public static method L(): int
    using var pool := new OwningLinkedList<W>();
    pool.AddLast(new W(1));
    using var view := pool.Drain();
    using var it := view.GetOwningEnumerator();
    return pool.Count;
  end L;
  public static method Main(): int
    return C.A() + C.Q() + C.S() + C.K() + C.L();
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: internal:
// NODIAG-NOT: #2434
