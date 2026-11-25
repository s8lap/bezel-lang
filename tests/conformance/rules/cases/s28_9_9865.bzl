//! case: s28_9_9865
//! doc: spec
//! section: 28.9
//! title: A generic abstract member with an iterator override reads a closure after the frame is returned
//! rule: Function-typed fields, parameters, and locals are not subject to `owned` annotations and carry no disposal obligation
//! cites: 28.9, 28.14, 29.4, 14.1
//! expect: run
//! stdout: 16800
//! notes: Harvested from the #9309 review (p12_generic_abstract). A GENERIC abstract member whose
//!   override is an iterator; the caller returns the frame, so it outlives the closure local, and
//!   the frame reads the callback on enumeration (29.4, 28.14 call-bounded x deferred activation).
//!   Trace: T = int, the frame yields 42 and 42 = 84 per round, 200 rounds = 16800.
module Conf;

public abstract class Fl
  public abstract method M<T>(q: () -> T): IEnumerable<T>;
end Fl;

public class Sub : Fl
  public override method M<T>(q: () -> T): IEnumerable<T>
    yield return q();
    yield return q();
  end M;
end Sub;

public class Program
  public static method Make(fl: Fl): IEnumerable<int>
    var c := 42;
    var g := lambda() => c;
    return fl.M<int>(g);
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
