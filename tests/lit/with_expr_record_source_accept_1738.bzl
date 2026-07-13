// Clean-compile ACCEPT control for issue #1738.
//
// Spec §9.5: "The source expression must be record-typed"; a `with`
// expression over a genuine record source (§9.5's own canonical example
// shape: `p with Y := 9 end`) must still compile clean after the enum-source
// gate fix — this proves the new Enum rejection does NOT over-reject the
// permitted record-typed source case.
//
// Two RUN lines assert the accept two ways:
//  1. `bezelc -c` must EXIT ZERO — lit fails the test on a non-zero return, so
//     a successful compile IS the pass. Output goes to `%t.o` (build-tree
//     scratch).
//  2. The diagnostic stream must be empty of errors — FileCheck with
//     `--allow-empty` confirms neither "error" nor a § section-citation reject.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
record Point(X: int, Y: int);
class C
  public static method Main(): int
    var p := new Point(1, 2);
    var q := p with Y := 9 end;
    return q.X + q.Y;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
