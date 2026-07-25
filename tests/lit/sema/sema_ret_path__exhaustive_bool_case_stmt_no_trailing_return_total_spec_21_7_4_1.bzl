// #2133 (§21.7 / §4.1 / §19.6): a BOOL scrutinee is a CLOSED VALUE SPACE — its
// value set is exactly `{true, false}`, and no `int`→`bool` cast exists (§4.1),
// so covering both literals is complete runtime coverage. An EXHAUSTIVE
// statement-`case` over a `bool` whose every arm diverges therefore has no
// reachable no-match fall-through edge (§19.6) and is TOTAL: the method may end
// with it, with no trailing `return`. This extends the #2129 choice totality
// grant to `bool`, superseding the v0.196 N6 ruling for closed-value-space kinds.
//
// Without the fix, §21.7 fired "does not return a value on every path"
// (WRONG-without) because the totality gate was restricted to choice scrutinees;
// with the fix, the bool bullet of §21.7 admits `bool` and the shared
// exhaustiveness verdict proves both literals covered, so no diagnostic fires
// (CORRECT-with).
//
// Cite spec §21.7, §4.1, §19.6 (v0.226).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class C
  public method Classify(b: bool): int
    case b of
      true  => return 1;,
      false => return 2;
    end;
  end Classify;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
