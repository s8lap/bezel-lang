// fix #4045 (codegen, LlvmBackend_Stmts.cpp Emitter::lowerStmt, v0.294):
// silent-success negative control for the AssignmentStmt generic lvalueAddr
// arm's new diagnostic at its null-address exit.
//
// #4045's NEW WARNING is gated on `lvalueAddr` returning null for the
// assignment target at the generic fallback (`if (!addr) return false;`).
// Today NO known sema-legal program reaches this path: every ordinary
// assignment shape (local, field, parameter, static field, auto-property,
// indexer, member access) is claimed by an earlier dedicated arm or
// resolves through `lvalueAddr`'s six resolution paths (narrowed
// identifier / ref-out param / local slot / MemberAccessExpr field /
// static field global / inline array element) with `implicitFieldAddr`
// catching the bare-identifier-instance-field shape as the seventh. The
// only way to hit the new diagnostic today is to introduce a new
// lowering gap (the case the fix is hardening against); pre-#4045 that
// gap would have shipped a silent no-op.
//
// THIS file is the silent-success negative control: every legal
// assignment shape the fix touches must still lower cleanly with NO
// `internal:` diagnostic at all (and NO `#4045` text specifically).
//
// Coverage:
//   * `local := 7`                          — plain scalar to a local
//   * `local := local + 1`                  — binary-op RHS
//   * `local := M(7)`                       — call-result RHS
//   * `this.field := 7`                     — plain scalar to a field
//   * `param := 7`                          — plain scalar to a parameter
//   * `arr[i] := v`                         — indexer-set assignment
//   * `nullable ??= v`                      — null-coalescing assignment
//   * `param += 1`                          — compound assign to parameter
//   * `this.field += 1`                     — compound assign to instance field
// All must lower real values and produce NO #4045 diagnostic (and NO
// #2961 / #3025 either — the sibling family is also clean on these shapes).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// `internal:` / `#4045` / `#2961` / `#3025` diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module N4045Lit;
class Inner4045 public constructor(v: int) _v := v; end constructor; public _v: int; destructor end destructor; end Inner4045;
class Holder4045
  public fieldA: int;
  public fieldB: int;
  public constructor() fieldA := 0; fieldB := 0; end constructor;
end Holder4045;
class C4045
  public static method Mk(): int return 42; end Mk;
  public static method Idx(arr: Array<int>, i: int): int return arr[i]; end Idx;
  public static method Main(p: int): int
    using var arr: Array<int> := new Array<int>(4, 0);
    var local: int := 1;
    var own: Inner4045 := new Inner4045(3);
    var n: int? := 5;
    var coalesced: int? := null;
    using var holder := new Holder4045();
    local := 7;
    holder.fieldA := 9;
    holder.fieldB := local + 1;
    local := C4045.Mk();
    local := C4045.Idx(arr, 0);
    local := p + 1;
    p := p + 2;
    arr[0] := 42;
    coalesced ??= 11;
    holder.fieldA += 1;
    p += 1;
    _ := own;
    return local + holder.fieldA + holder.fieldB + (coalesced ?? 0) + p;
  end Main;
end C4045;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: #4045
// NODIAG-NOT: #2961
// NODIAG-NOT: #3025
