//! case: s28_9_9862
//! doc: spec
//! section: 28.9
//! title: A closure passed to an abstract member with an iterator override survives two enumerations
//! rule: Function-typed fields, parameters, and locals are not subject to `owned` annotations and carry no disposal obligation
//! cites: 28.9, 28.14, 29.4, 27.5
//! expect: run
//! stdout: 2600
//! notes: Harvested from the #9309 review (r43-rev-m147-9309, p04_twice_reuse). A closure local is
//!   passed to an ABSTRACT member whose override is an iterator; the returned frame is enumerated
//!   twice (29.3: an `IEnumerable<T>` iterator is re-iterable) with the captured variable changed
//!   between the passes (27.5: modifications to a captured variable are visible to the closure),
//!   and the closure is still callable afterwards. The frame holds the callback for its own life
//!   (29.4, 28.14), the caller's local keeps its own reference. Trace per round: c = 1, then c := 2
//!   and the first pass yields 2 + 2 = 4; c := 3 and the second pass yields 3 + 3 = 6; g() = 3;
//!   13 per round, 200 rounds = 2600.
module Conf;

public abstract class Fl
  public abstract method M(q: () -> int): IEnumerable<int>;
end Fl;

public class Sub : Fl
  public override method M(q: () -> int): IEnumerable<int>
    yield return q();
    yield return q();
  end M;
end Sub;

public class Program
  public static method Main(): int
    var t := 0;
    var fl: Fl := new Sub();
    for var j in 0 ..< 200 do
      var c := 1;
      var g := lambda() => c;
      using var e := fl.M(g);
      c := c + 1;
      for var x in e do t := t + x; end;
      c := c + 1;
      for var x in e do t := t + x; end;
      t := t + g();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
