// fix #3329 / proposal 0116 §3 D2 (spec §14.2, stdlib §4.4, v0.276):
// `Array<T>.Sort()`/`Sorted()`'s rejection diagnostic must name the
// SUBSTITUTED type argument, not the unsubstituted `T`, and must cite
// the current stdlib section (not retired §3).
//
// The three defects this pins, against proposal 0116 D2's four-part
// diagnostic contract for conditional members:
//
//   1. The diagnostic prints unsubstituted `T` — telling the user the
//      rule but not WHICH of their types broke it. After the fix it
//      spells out `Array<Widget>` and `Widget does not implement
//      IComparable<Widget>` so the reader can trace the failure to a
//      concrete type.
//
//   2. The diagnostic cites retired stdlib §3. The live home is
//      stdlib §4.4 (the declarations and the condition) and spec §14.2
//      (the constraint vocabulary). The CHECK-NOT lines pin both: the
//      new message names §4.4 and §14.2, and does NOT name retired §3.
//
//   3. The diagnostic states an accept-list ("is not a built-in
//      comparable type") rather than the structural reason. The
//      structural reason is that the substituted type does not
//      implement `IComparable<Self>`; the fix states that directly.
//
// The rejection site is `src/sema/ExprTypecheck_Calls.cpp:5622-5635`.
// The acceptor control (a class implementing `IComparable<Self>` sorts
// correctly) is the load-bearing half of the fix — a typo that rejected
// all classes would still pass these CHECK lines — and lives in
// `tests/codegen/array_sort_constraint_gate_2790_test.cpp`.
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M3329Diag;

// `Widget` declares no `IComparable<Self>` so `Array<Widget>.Sort()` is
// rejected at the call site per stdlib §4.4 / spec §14.2 rule 1.
class Widget
  public constructor() end constructor;
  public method Tag(): int => 1;
end Widget;

class C
  public static method Main(): int
    using var a := new Widget();
    var arr: Array<Widget> := [a];
    arr.Sort();
    return 0;
  end Main;
end C;

// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: §3 / #472
// CHECK-NOT: Array<T>.Sort() requires
// (1) The receiver is named with the substituted type argument — the
//     defect this file's primary fix is to remove.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Array<Widget>.Sort() requires T: IComparable<T>
// (2) The constraint-vocabulary section (spec §14.2) is cited.
// CHECK-SAME: spec §14.2
// (3) The current stdlib section is cited — stdlib §4.4, not retired §3.
// CHECK-SAME: stdlib §4.4
// (4) The structural reason names the substituted type in the
//     constraint position, not "T is not a built-in comparable type".
// CHECK-SAME: Widget does not implement IComparable<Widget>
// (5) The remedy points at the always-admitted comparer overload for
//     the form the user actually wrote (`Sort`, not `Sorted`).
// CHECK-SAME: Use Sort(comparer: (Widget, Widget) -> int) instead.
// (6) The retired §3 reference is NOT in the diagnostic. It is retired
//     and would mislead the reader into a nonexistent section.
// CHECK-NOT: §3 / #472
// (7) The unsubstituted `T` is NOT printed in the receiver position
//     (the first defect). `Array<T>.` would tell the user nothing.
// CHECK-NOT: Array<T>.Sort() requires
