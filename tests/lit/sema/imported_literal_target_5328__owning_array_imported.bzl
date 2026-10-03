// RUN: %bezelc -c -o %t.lib.o %S/imported_literal_target_5328__owning_array_imported__lib.bzl
// RUN: %bezelc -c %s --import %t.lib.o
// RUN: %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #5328 / spec §15.4 (v0.313) — CONTROL ROW for the issue's item 4: a
// BORROWED `OwningArray<Box>` formal, imported, with fresh `new` elements.
//
// §15.4 puts "`Array<T>` (heap-borrowing) and `OwningArray<T>` (heap-owning)"
// on the closed target list, and for an OWNING target "each element
// expression must produce owned values" — a `new` expression is exactly
// that. So the literal is WELL-TYPED here and must be admitted: there is no
// §15.4 diagnostic to give, in either leg.
//
// This row is the typing half of item 4 and nothing more, and the limit is
// deliberate. The issue measures that the IMPORTED leg LEAKS the two `Box`
// objects per call under ASan where the same-module twin does not — the
// disposer stamp / element disposal being derived from an `importedName`
// TypeRef rather than the `astNode` one. That is a CODEGEN ownership defect
// (an `owning` formal is fine, so it is the borrowed family + cross-boundary
// element disposal), it is only observable by RUNNING the program under
// LeakSanitizer, and this lane changes no codegen — so this row does NOT
// claim the leak is fixed and does not pretend a compile-time check could
// see it. It pins what IS this lane's: the call is admitted, and the
// semantically-valid target §15.4 names is not rejected. A follow-up codegen
// lane carries the leak; see the commit body.
//
// Two RUN lines assert the accept two ways, per xmod_static_interface_accept_2294.
module App5328OAb;

import OAbLib5328;

public class Program
  public static method Main(): int
    Console.WriteLine(
      OAbLib5328.OabOps.OAb([new OAbLib5328.Box(1), new OAbLib5328.Box(2)]));
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: cannot target
// NODIAG-NOT: §15.4
// NODIAG-NOT: internal:
