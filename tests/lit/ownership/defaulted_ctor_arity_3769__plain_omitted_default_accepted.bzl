// RUN: %bezelc -c -o %t.o %s
//
// Issue #3769, ANTI-OVER-REJECT. The fix newly RESOLVES a formal list for a
// call that omits a §22.5 default, which makes every consumer of that list
// newly reachable for this shape — the owned-sink classifiers, the §28.14
// call-bounded and `scoped` gates, and the §31.1 catch-borrow scan. A §22.10
// primary whose parameters are all PLAIN must stay exactly as it was: nothing
// here is an owned sink and nothing is rejected.

module M;

class P(a: int, b: int := 1)
  public method Sum(): int
    return a + b;
  end Sum;
end P;

class C
  public static method Main(): int
    var p := new P(2);
    return p.Sum();
  end Main;
end C;
