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
// evidence rather than reasoning: the guard below that forbids the "cannot
// target the interface type" wording FAILS if the name-match branch ever
// starts firing on a user class.
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

// #7071: this row's expected diagnostic is emitted by CODEGEN — the
// imported-argument lowering of `lowerCallDispatch`
// (src/codegen/LlvmBackend_Calls2.cpp:14276) — so CODEGEN RUNS on this row.
// The ICE-escape guard must therefore name a marker that layer can produce:
// `internal:` is the codegen-only marker (every `internal:` string literal in
// src/ lives under src/codegen/), so it is the one that can fire here.
//
// #6620's replacement — the front end's own `internal compiler error` —
// cannot land on this row: all four front-end ICE sites are `[[noreturn]]`
// aborts that kill the process before the positive diagnostic exists, so the
// needle was never even evaluated. #6620's premise ("all 17 rows reject in
// the front end") is false for this row.
//
// The guard is written on BOTH sides of the positive match: a codegen
// `internal:` diagnostic is collected and rendered at the end of the compile
// (src/driver/Driver.cpp:809), in report order, so it can land either before
// or after the §15.4 diagnostic this row pins.
// CHECK-NOT: internal:
// #7070: the two wording guards below say "the §15.4 SEMA wording must not
// appear in this output" — and that is a claim about the WHOLE output, not
// only about the text that follows the expected diagnostic. A `CHECK-NOT`
// scans the region between the surrounding matches, so the post-positive
// copy alone lets a wrong diagnostic emitted FIRST through. These are the
// pre-positive copies: same needle, region before the positive match.
// CHECK-NOT: cannot target the interface type
// CHECK-NOT: cannot target the type
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a collection literal `[...]` cannot target the imported parameter type here
// The load-bearing assertions: the gate must NOT have called this class an
// interface, and must NOT have reached the generic out-of-list diagnostic.
// CHECK-NOT: cannot target the interface type
// CHECK-NOT: cannot target the type
// CHECK-NOT: internal:
