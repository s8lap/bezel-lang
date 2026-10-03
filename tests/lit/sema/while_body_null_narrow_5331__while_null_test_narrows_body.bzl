// Fix #5331 (spec §7.4, v0.313), pin row 1 of the while-body arm: a null
// test in a `while` condition narrows that arm's BODY.
//
// §7.4 "How narrowing is established" names the establishing positions
// explicitly: "a null test — `x <> null` (narrowing the true branch) … — in an
// `if`/`elif` condition …, a `while` condition, or a `when` guard". Before the
// fix the `WhileStmt` arm of `ExprTC::visitStmt` never called
// `collectNarrowings` on its condition, so `a` kept its declared `string?`
// type through the whole body and `a.Length` was wrongly rejected.
//
// The while CONDITION's true-facts govern the body (the body runs only when the
// condition holds). The after-loop point is a different program point and is
// NOT narrowed by this fact — the zero-iteration path leaves `a` possibly null;
// that edge is pinned by the companion reject track
// while_body_null_narrow_5331__condition_fact_does_not_survive_loop.bzl.
//
// Sema-level only (compile, no run).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class C
  public static method G(): string? return "ab"; end G;

  // The issue #5331 repro, verbatim in shape: the body's `.Length` is the read
  // the condition licenses.
  public static method Main(): int
    var a := C.G();
    while a <> null do
      Console.WriteLine(a.Length);
    end;
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
