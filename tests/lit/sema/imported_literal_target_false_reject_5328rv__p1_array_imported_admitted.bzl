// RUN: %bezelc -c -o %t.lib.o %S/imported_literal_target_false_reject_5328rv__lib.bzl
// RUN: %bezelc -c %s --import %t.lib.o
// RUN: %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Review row for issue #5328 / spec §15.4 (v0.313) — FALSE-REJECT PROBE 1 of
// 5: an `Array<int>` formal, imported, with a sequence literal.
//
// §15.4 puts "`Array<T>` (heap-borrowing) and `OwningArray<T>` (heap-owning)"
// on the closed list of targets that admit a collection literal, so `Arr([1,
// 2, 3])` at `Arr(x: Array<int>)` is well-typed and must compile clean in BOTH
// legs. The new imported-channel gate runs
// `isCollectionLiteralInterfaceTarget` on the formal; `Array<int>` is a
// CONCRETE class, not one of the ten collection-interface simple names, so the
// predicate must answer false and admit the call.
//
// Why this row is not redundant with the producer's
// `imported_literal_target_5328__set_imported_admitted.bzl`: that row covers
// `Set<int>`, this one covers a different builtin head (`Array`), and the
// predicate is a NAME match, so breadth over the concrete-collection names is
// the property under test — a fix that accidentally matched a prefix, or that
// reached the generic out-of-list diagnostic for a null-`astNode` head, would
// pass the `Set` row and fail here.
//
// Two RUN lines assert the accept two ways, per the
// xmod_static_interface_accept_2294 pattern — the compile must EXIT ZERO, and
// the diagnostic stream must contain no error.
module FRP1;

import FR5328Lib;

public class Program
  public static method Main(): int
    Console.WriteLine(FR5328Lib.FROps.Arr([1, 2, 3]));
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: cannot target
// NODIAG-NOT: §15.4
// NODIAG-NOT: internal:
