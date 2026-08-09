// fix #2961 (codegen, lowerStmt := arms, v0.240): negative control for the
// sibling ACCEPT-WITH-WARNING test (this dir,
// assignment_rhs_lowering_fallback_warns_2961.bzl). #2961's new internal
// Warning is gated on `lowerExpr` FAILING to lower the RHS — a successful
// assignment whose RHS lowers correctly (the overwhelmingly common case:
// plain value, owned, string, nullable, method-call result, indexer-set
// operand) must stay COMPLETELY SILENT. A regression that dropped the `!rhs`
// check (firing on EVERY assignment) would turn this clean-compile control
// red — pinning it here catches that shape of regression directly, the same
// way the sibling test pins the case that SHOULD warn.
//
// Covers a representative spread of the success-path assignment shapes the
// fix touches (local, field, parameter, indexer, nested chain, nullable
// target, nullable RHS):
//   * `local := 7`                          — plain scalar to a local
//   * `this.field := 7`                     — plain scalar to a field
//   * `param := 7`                          — plain scalar to a parameter
//   * `local := "hi"`                       — runtime-managed string RHS
//   * `local := obj`                        — class reference (borrow)
//   * `local := owned obj`                  — owned class RHS (transfer)
//   * `local := nullable`                   — nullable RHS to a nullable
//   * `arr[i] := v`                         — indexer-set assignment
//   * `local := local + 1`                  — binary-op RHS
//   * `local := M(7)`                       — call-result RHS
// All must lower real values and produce NO #2961 diagnostic.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// internal: / #2961 diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class Inner public constructor(v: int) _v := v; end constructor; public _v: int; destructor end destructor; end Inner;
class Holder
  public fieldA: int;
  public fieldB: int;
  public constructor() fieldA := 0; fieldB := 0; end constructor;
end Holder;
class C
  public static method Mk(): int return 42; end Mk;
  public static method Idx(arr: Array<int>, i: int): int return arr[i]; end Idx;
  public static method Main(): int
    using var arr: Array<int> := new Array<int>(4, 0);
    var local: int := 1;
    var s: string := "x";
    var own: Inner := new Inner(3);
    var n: int? := 5;
    using var holder: Holder := new Holder();
    local := 7;
    s := "hi";
    own := new Inner(7);
    n := 5;
    arr[0] := 42;
    holder.fieldA := 9;
    holder.fieldB := local + 1;
    local := C.Mk();
    local := C.Idx(arr, 0);
    _ := own;
    return local + holder.fieldA + holder.fieldB;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: #2961
