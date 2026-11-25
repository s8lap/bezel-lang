//! case: s28_9_9864
//! doc: spec
//! section: 28.9
//! title: A non-iterator override of an abstract member taking a closure reads it and returns the values
//! rule: Function-typed fields, parameters, and locals are not subject to `owned` annotations and carry no disposal obligation
//! cites: 28.9, 28.14, 12.5
//! expect: run
//! stdout: 16800
//! notes: Harvested from the #9309 review (p11_noniter_override). The admitting twin of the iterator
//!   override: the same ABSTRACT member, overridden by an ordinary method that calls the callback
//!   twice and returns an `Array<int>` as the `IEnumerable<int>`. No frame is minted, so the
//!   callback is call-bounded in the plain sense; the closure local dies at the end of `Make` and
//!   the returned values were already read. Trace: 42 + 42 = 84 per round, 200 rounds = 16800.
module Conf;
import System.Collections;

public abstract class Fl
  public abstract method M(q: () -> int): IEnumerable<int>;
end Fl;

public class Sub : Fl
  public override method M(q: () -> int): IEnumerable<int>
    var a := new Array<int>();
    a.Add(q());
    a.Add(q());
    return a;
  end M;
end Sub;

public class Program
  public static method Make(fl: Fl): IEnumerable<int>
    var c := 42;
    var g := lambda() => c;
    return fl.M(g);
  end Make;

  public static method Main(): int
    var t := 0;
    var fl: Fl := new Sub();
    for var j in 0 ..< 200 do
      for var x in Make(fl) do t := t + x; end;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
