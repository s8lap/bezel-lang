// #2133 (§21.7 / §19.6): the irrefutable-arm bullet of the statement-case
// totality rule admits an unguarded `_` OR an unguarded untyped `var x` — "one
// proof in both spellings" (§19.6). Either is a catch-all that matches every
// value of the scrutinee, so it closes the no-match fall-through edge for ANY
// scrutinee kind (not only the closed-value-space kinds). #2133 widened the
// gate from `_`-only to also admit the var-spelled irrefutable arm, so an
// untyped `var x` closes totality uniformly regardless of how the programmer
// wrote the catch-all.
//
// Here the scrutinee is an `int` (an OPEN value space — no closed-set proof is
// available), but the unguarded untyped `var x` arm covers every value the
// preceding `0 =>` arm does not, and both arms diverge (return); so the case is
// total and the method needs no trailing return.
//
// Without the fix, §21.7 fired "does not return a value on every path"
// (WRONG-without) because the catch-all gate recognized only `_`, not untyped
// `var x`; with the fix, the isUnguardedCatchAll helper admits both spellings
// and totality closes (CORRECT-with).
//
// Cite spec §21.7, §19.6 (v0.226).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class C
  public method Classify(n: int): int
    case n of
      0 => return 0;,
      var x => return x;
    end;
  end Classify;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
