// fix #2491 (codegen, return-path twin of #2434, v0.236): negative control
// for the sibling ACCEPT-WITH-WARNING test (this dir,
// return_value_lowering_fallback_warns_2491.bzl). #2491's new internal
// Warning is gated on the return expression FAILING to lower — a return
// whose value lowers correctly (the overwhelmingly common case: plain
// value, string, owned, nullable, arrow-body, property getter) must stay
// COMPLETELY SILENT. A regression that dropped the `!v` check (firing on
// EVERY return) would turn this clean-compile control red — pinning it
// here catches that shape of regression directly, the same way the sibling
// test pins the case that SHOULD warn.
//
// Covers a representative spread of the success-path return shapes the fix
// touches (ReturnStmt, arrow body `=> expr`, and a property getter body):
//   * `return 42;`            — plain scalar ReturnStmt (Stmts.cpp site)
//   * `=> o`                  — value-bearing arrow-body return (Setup.cpp site)
//   * `property N: int => _v` — property getter body (Assign.cpp site)
//   * `return new Inner(7);`  — owned-class ReturnStmt
//   * `return C.V();`         — call-result ReturnStmt
// All must lower real values and produce NO #2491 diagnostic.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// internal: / #2491 diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class Inner public constructor(v: int) _v := v; end constructor; public _v: int; destructor end destructor; public property N: int => _v; end Inner;
class C
  public static method V(): int return 42; end V;
  public static method Arrow(o: Object): Object => o;  // value-bearing arrow
  public static method Owned(): owned Inner return new Inner(7); end Owned;
  public static method Main(): int using var o := C.Owned(); var n := (new Inner(3)).N; return C.V(); end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: #2491
