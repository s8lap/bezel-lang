// RUN: %bezelc -c -o %t.lib.o %S/imported_literal_target_false_reject_5328rv__lib.bzl
// RUN: %not %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck %s
//
// Review row for issue #5328 / spec §15.4 (v0.313) — FALSE-REJECT PROBE 5 of
// 5: a USER-DECLARED CLASS whose simple name is `IList`, the exact spelling
// of the builtin collection INTERFACE `IList<T>`.
//
// This is the sharpest false-reject the new gate could plausibly introduce,
// and it is the row that separates the predicate's two branches from the
// outside. `isCollectionLiteralInterfaceTarget` matches a BUILTIN collection
// interface by SIMPLE NAME through `callsNamedTypeSimpleName`, which is
// exactly the spelling an imported formal carries. A user class named `IList`
// arrives at that same function from across the module boundary, and a
// name-only implementation of this predicate would call it an interface and
// reject it. It must not: the class below DECLARES the §15.4.1
// sequence-literal protocol, so it is a legitimate literal target, and the
// predicate answers on the SEMANTIC fact (`t->declaration->kind` is Class,
// not Interface) rather than on the spelling.
//
// The shipped fix gets this right, and this row is what makes that claim
// evidence rather than reasoning: the `CHECK-NOT: cannot target the interface
// type` line below FAILS if the name-match branch ever starts firing on a
// user class.
//
// The program IS still rejected, and the rejection is NOT this lane's. It
// comes from codegen's `lowerCallDispatch` imported-argument arm with
// "cannot target the imported parameter type here" — the same residual the
// producer's `inline_array_imported` row pins, and the same issue #5328 item 2
// that the follow-up codegen lane carries. So the row asserts exactly that:
// the CODEGEN string is present, and NEITHER §15.4 sema string is. The
// `%not` + `CHECK` combination is deliberate for the same reason it is in
// `inline_array_imported` — and when the codegen arm admits this target, the
// `CHECK` will fail, which is the intended revisit signal.
module FRP5;

import FR5328Lib;

public class Program
  public static method Main(): int
    Console.WriteLine(FR5328Lib.FROps.TakesIList([1, 2, 3]));
    return 0;
  end Main;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a collection literal `[...]` cannot target the imported parameter type here
// The load-bearing assertions: the gate must NOT have called this class an
// interface, and must NOT have reached the generic out-of-list diagnostic.
// CHECK-NOT: cannot target the interface type
// CHECK-NOT: cannot target the type
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
