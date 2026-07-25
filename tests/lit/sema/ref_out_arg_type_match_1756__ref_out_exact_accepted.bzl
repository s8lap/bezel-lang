// Clean-compile ACCEPT control for §22.3 (v0.226) / #1756: when the `ref`
// and `out` arguments' own static type EXACTLY matches the parameter's
// referent type, the call must still compile clean — the new exact-type
// check must not over-reject the matching case. This mirrors the structure
// of the two reject neighbors (Inc(ref x: int) / Fill(out x: int)) but
// passes an `int` recipient to each, so the §22.3 no-conversion rule is
// satisfied trivially and no diagnostic appears.
//
// Two RUN lines assert the accept two ways:
//  1. `bezelc -c` must EXIT ZERO — lit fails the test if the command returns
//     non-zero, so a successful compile IS the pass.  Output goes to `%t.o`
//     (build-tree scratch), never the source tree.
//  2. The diagnostic stream must be empty of errors — FileCheck with
//     `--allow-empty` (there is no output) confirms neither "error" nor a §
//     section-citation reject appears.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
class C
  public static method Inc(ref x: int) x := x + 1; end Inc;
  public static method Fill(out x: int) x := 42; end Fill;
  public static method Main(): int
    var n: int := 5;
    C.Inc(ref n);
    var m: int := 0;
    C.Fill(out m);
    return n + m;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
