// Clean-compile ACCEPT control for issue #1653 — the KEYED-TRANSIT EXCEPTION.
//
// Spec §28.6 (anchor sec-who-may-dispose, v0.184) "The transit exception
// (recorded)": an element borrow obtained from a BORROWING-family collection —
// an indexer-get, a default-iteration variable, dictionary-entry fields, or any
// other element read in the transit-point regime of §28.11 — MAY be disposed. A
// borrowing collection is a transit point whose elements' true owner is
// structurally unknowable, so disposing semantically-owned elements through it
// is the recorded escape hatch (the manual form of `DisposeElements()`). The
// exception is keyed on the SOURCE expression's static type: only an element
// read whose receiver is statically a borrowing-family collection
// (Array/Dictionary/Set/Stack/Queue/LinkedList) is in the transit regime.
//
// Here `arr` has static type `Array<Resource>` (a borrowing-family collection),
// so both the default-iteration variable `e` and the bare indexer-get `arr[0]`
// are transit-eligible: `e.Dispose()` and `arr[0].Dispose()` must compile clean.
// This is the carve-out that keeps the §28.6 receiver check from over-rejecting
// the legitimate transit pattern — the counterpart to the lazy-query REJECT the
// issue cites (`for var x in xs.Where(p) do x.Dispose()`), whose source is an
// IEnumerable<T> and is NOT keyed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
class Resource public method Ping(): () end Ping; end Resource;
class Program
  public static method Main(): int
    using var arr := new Array<Resource>();
    arr.Add(new Resource());
    for var e in arr do
      e.Dispose();
    end;
    arr[0].Dispose();
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
