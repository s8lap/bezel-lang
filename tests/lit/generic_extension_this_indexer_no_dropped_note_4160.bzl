// fix #4160 (§26.5 "Specificity ordering" worked example, §26.1, v0.338): a
// GENERIC extension method over a builtin generic target whose body reads
// `this[0]` must compile WITHOUT the `internal: this `this[i]` indexer access
// was DROPPED` announcement (#2833). The announcement fired from the
// unspecialized template body of `First` (lowered with `T` unbound only to
// keep the module's IR valid; never called), while every specialization
// recovered the receiver and ran correctly. Two element types are
// instantiated so a specialization that failed to recover the receiver would
// still announce (the suppression is gated on `T` being UNBOUND).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
extension Array<T>
  public method First(): T
    if this.Count = 0 then throw new InvalidOperationException("empty array"); end;
    return this[0];
  end First;
  public method SetFirst(v: T)
    this[0] := v;
  end SetFirst;
  public method Swap01()
    var t := this[0];
    this[0] := this[1];
    this[1] := t;
  end Swap01;
end;
extension Dictionary<K, V>
  public method Peek(k: K): V => this[k];
end;

public static class Program
  public static method Main(): int
    var xs: Array<int> := [1, 2, 3];
    var ys: Array<string> := ["a", "b"];
    xs.SetFirst(9);
    xs.Swap01();
    var d := new Dictionary<string, int>();
    d["a"] := 5;
    Console.WriteLine(xs.First().ToString() + ys.First() + d.Peek("a").ToString());
    return 0;
  end Main;
end;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: DROPPED
