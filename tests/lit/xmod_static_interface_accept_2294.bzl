// RUN: %bezelc -c -o %t.lib.o %S/xmod_static_interface_2294_lib.bzl
// RUN: %bezelc -c -o %t.o %s --import %t.lib.o
// RUN: %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// (`%S` = this test file's directory, so the first RUN line builds the
// dependency artifact the other two import.)
//
// Issue #2294 / spec §11.8 (v0.176), §11.2, §2.8.2 — a class implementing an
// IMPORTED interface's `static` members must be ACCEPTED when its witnesses
// match. Before the fix this was rejected unconditionally: the conformance
// check (`Resolver::checkStaticInterfaceConformance`) read the interface
// member's signature only from its AST declaration, and an imported member has
// no syntactic AST — its return type rides SemaContext's imported side table
// and its parameters carry `ParameterSymbol::resolvedDeclType`. The null
// return node was conflated with "the interface member returns unit", so a
// value-returning witness never matched, and the null parameter type made the
// param loop bail. §11.8 conformance was single-module-only in practice.
//
// Two RUN lines assert the accept two ways, per the ref_out_local_accept_1682
// pattern: the compile must EXIT ZERO, and the diagnostic stream must contain
// no error. This is the ACCEPT control for the sibling reject test, which pins
// that a genuine mismatch is still diagnosed.
module SiApp;
import SiLib2294;

public class Doubler: IPing
  public static method Ping(n: int): int => n * 2;
end Doubler;

public class Toucher: IVoid
  public static _hit: int := 0;
  public static method Touch()
    Toucher._hit := 5;
  end Touch;
end Toucher;

public class C
  public static method Main(): int => Doubler.Ping(21);
end C;

// NODIAG-NOT: error
// NODIAG-NOT: different signature
// NODIAG-NOT: does not implement static interface member
