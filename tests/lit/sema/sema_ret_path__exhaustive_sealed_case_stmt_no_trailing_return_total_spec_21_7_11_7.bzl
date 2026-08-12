// #2133 (§21.7 / §11.7 / §19.6): a `closed` class hierarchy is a
// CLOSED VALUE SPACE — every instance is one of the closed concrete-leaf set, and
// no out-of-hierarchy instance can exist (§11.7) — so an EXHAUSTIVE statement-
// `case` over it whose every arm diverges has no reachable no-match fall-through
// edge (§19.6: in statement position an unmatched value falls through with no
// action) and is TOTAL: the method may end with it, with no trailing `return`.
// This extends the #2129 choice totality grant to `closed`-rooted
// hierarchies, superseding the v0.196 N6 ruling for closed-value-space kinds.
//
// This program is byte-for-byte the reject test's `Classify` (Shape / Circle /
// Square, both type-test arms `return`) with NO trailing return: it now compiles
// clean. Without the fix, §21.7 fired "does not return a value on every path"
// (WRONG-without); with the fix, the exhaustive sealed case-statement closes
// totality and no diagnostic fires (CORRECT-with). The exhaustiveness verdict is
// reused from the expression-form check (caseArmsExhaustive), so the same
// `closed` gate that proves the expression exhaustive also grants statement
// totality here.
//
// Cite spec §21.7, §11.7, §19.6 (v0.226).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
closed class Shape
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
  end Classify;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
