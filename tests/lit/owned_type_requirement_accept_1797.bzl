// Clean-compile ACCEPT control for issue #1797 (spec §28.1 "Type requirement",
// v0.193; bare-type-parameter arm v0.196).
//
// §28.1 ADMITS `owned` on a method parameter, return type, or `out` parameter
// whose type (nullability stripped) is a class type, an interface type, or a
// bare type parameter. The #1797 fix must not false-reject any of these — the
// whole point of `owned` is class/interface/type-parameter resources. A bare
// type parameter is admitted at the declaration site UNCONDITIONALLY (the
// per-instantiation erasure of the marker at value/runtime-managed
// substitutions, v0.196, is tracked separately as #1817).
//
// Asserted two ways:
//  1. `%bezelc -c` must EXIT ZERO — a successful compile is the pass; output
//     goes to build-tree scratch (`%t.o`), never the source tree.
//  2. The diagnostic stream must contain neither an `error` nor any section
//     citation (`§`) — no false §28.1 reject and no incidental warning.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
interface IWork
  method Go(): ();
end IWork;
class Conn
  public constructor() end constructor;
  public method Go(): () return; end Go;
end Conn;
class R
  // owned CLASS parameter — discharged by Dispose().
  public method A1(owned c: Conn): () c.Dispose(); return; end A1;
  // owned INTERFACE parameter — discharged by Dispose().
  public method A2(owned w: IWork): () w.Dispose(); return; end A2;
  // owned BARE TYPE PARAMETER (constrained `where T: class`) — transferred out
  // via an owned bare-type-parameter return.
  public method A3<T>(owned x: T): owned T where T: class return x; end A3;
  // owned BARE TYPE PARAMETER, no constraint — admitted at the declaration
  // site (erasure deferred to #1817); transferred out.
  public method A4<T>(owned x: T): owned T return x; end A4;
  // owned CLASS return type.
  public method A6(): owned Conn return new Conn(); end A6;
  // owned NULLABLE class parameter (`C?`) — nullability is stripped for the
  // requirement, so the class type is admitted.
  public method A7(owned c: Conn?): () c!.Dispose(); return; end A7;
end R;

// NODIAG-NOT: error
// NODIAG-NOT: §
