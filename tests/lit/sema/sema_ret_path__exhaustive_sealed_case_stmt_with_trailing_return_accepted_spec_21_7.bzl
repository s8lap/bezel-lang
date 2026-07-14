// #1816 (s9) — §19.6 v0.196 / §21.7: ACCEPT control for the reject sibling
// (sema_ret_path__exhaustive_sealed_case_stmt_no_trailing_return_diagnoses_spec_19_6_21_7.bzl).
//
// This program is byte-for-byte the reject test's `Classify` with ONE addition:
// a trailing `return 0;` after the exhaustive `case`. It compiles clean. Pairing
// the two isolates the missing trailing return as the *sole* cause of the §21.7
// diagnostic: the §19.6-exhaustive case (all arms return, no `_`) is well-formed
// and the case itself is not the defect — totality simply does not treat the
// §19.6 proof as an exit on every path (v0.196 N6, "Totality must not key on the
// proof"). If a refactor ever made the trailing return *unreachable-dead* under
// an exhaustiveness proof, this accept would break too.
//
// Cite spec §19.6, §21.7 (v0.196).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
sealed abstract class Shape
  public abstract method Tag(): int;
end Shape;
sealed class Circle : Shape
  public override method Tag(): int => 1;
  destructor end destructor;
end Circle;
sealed class Square : Shape
  public override method Tag(): int => 2;
  destructor end destructor;
end Square;
class C
  public method Classify(s: Shape): int
    case s of
      var c: Circle => return 1;,
      var q: Square => return 2;
    end;
    return 0;
  end Classify;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
