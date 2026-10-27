// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §15.4 (#1866): a collection literal bound directly to a LOCAL declared as a
// collection INTERFACE (`IEnumerable<T>`) is not admissible — §15.4's target
// list is a CLOSED set of concrete collection types, and an interface has no
// concrete backing the literal could materialize into. This is the exact repro
// from issue #1866: the element type is a CONCRETE `Array<int>` (not a view),
// and before this fix the un-rejected literal reached codegen with a `ptr`-typed
// target and emitted MALFORMED IR — an "Invalid indices for GEP" verifier abort
// (a compile-time ICE), NOT a clean diagnostic. The rejection closes the same
// §15.4 gap already closed for the call-argument (#1174) and lambda
// expression-body (#1693) positions; the local-declaration position was missed.

module M;
class P
  public static method Main(): int
    var a: Array<int> := [1, 2, 3];
    var v: IEnumerable<Array<int>> := [a];
    var total: int := 0;
    for var row in v do
      for var x in row do total := total + x; end;
    end;
    return total;
  end Main;
end P;

// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: Invalid indices for GEP
// CHECK-NOT: IR verification failed
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a collection literal `[...]` cannot target the interface type `IEnumerable` — §15.4 admits only a concrete collection target (an inline array, `Array<T>`/`OwningArray<T>`, `Set`, `Stack`, `Queue`, `LinkedList`, `Dictionary`); construct one of those and pass it
// CHECK-SAME: (§15.4)
//
// And the malformed-IR ICE must NOT appear — the program is rejected in sema,
// long before codegen.
// CHECK-NOT: Invalid indices for GEP
// CHECK-NOT: IR verification failed
