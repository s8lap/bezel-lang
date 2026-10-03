// RUN: %bezelc -c -o %t.lib.o %S/imported_literal_target_false_reject_5328rv__lib.bzl
// RUN: %bezelc -c %s --import %t.lib.o
// RUN: %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Review row for issue #5328 / spec §15.4 (v0.313) — FALSE-REJECT PROBE 2 of
// 5: an `OwningSet<Box>` formal, imported, with a DUPLICATE literal element.
//
// §15.4 puts "`Set<T>` and `OwningSet<T>` (sets)" on the closed target list,
// and for that family "duplicates are collapsed". So a duplicate element is
// not a typing error at the call site — collapsing is the construction's job.
//
// This row is the OWNING spelling of the producer's
// `imported_literal_target_5328__set_imported_admitted.bzl`, and it is
// distinct from it on the axis that matters here. The producer's row proves
// the gate admits a bare `Set<int>`; this one proves it admits
// `OwningSet<Box>` — a name carrying the `Owning` prefix and a CLASS-typed
// (`where T: class`) element. A name matcher that keyed on a prefix, or that
// read the element type rather than the container, would separate the two.
//
// The `new Box(...)` elements are what force the `where T: class` binding, and
// they are also issue #5328's item-4 shape (a borrowed owning collection whose
// elements are fresh producers) — so this row doubles as a second instance of
// the producer's `owning_array_imported` typing control. As there, the
// COMPILES-CLEAN claim is a TYPING claim only: item 4's LeakSanitizer-
// observable leak is a codegen defect this lane does not touch, and no
// compile-time check can see it.
//
// Two RUN lines assert the accept two ways, per
// xmod_static_interface_accept_2294.
module FRP2;

import FR5328Lib;

public class Program
  public static method Main(): int
    Console.WriteLine(
        FR5328Lib.FROps.OSt([new Box(1), new Box(2), new Box(1)]));
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: cannot target
// NODIAG-NOT: §15.4
// NODIAG-NOT: internal:
