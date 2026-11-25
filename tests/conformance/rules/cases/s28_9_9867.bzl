//! case: s28_9_9867
//! doc: spec
//! section: 28.9
//! title: An interface member with an iterator implementation reads a closure after the frame is returned
//! rule: Function-typed fields, parameters, and locals are not subject to `owned` annotations and carry no disposal obligation
//! cites: 28.9, 28.14, 29.4, 11.2
//! expect: run
//! stdout: 16800
//! notes: Harvested from the #9309 review (the INTERFACE-member twin of p12; gtest row 2 of the
//!   fix). An interface member has no body (11.2), so a caller bound to the interface sees only the
//!   declaration; the implementing class provides an iterator. The caller returns the frame, so it
//!   outlives the closure local. Trace: the frame yields 42 and 42 = 84 per round, 200 rounds =
//!   16800.
module Conf;

public interface IProvider
  method M(q: () -> int): IEnumerable<int>;
end IProvider;

public class Impl : IProvider
  public method M(q: () -> int): IEnumerable<int>
    yield return q();
    yield return q();
  end M;
end Impl;

public class Program
  public static method Make(p: IProvider): IEnumerable<int>
    var c := 42;
    var g := lambda() => c;
    return p.M(g);
  end Make;

  public static method Main(): int
    var t := 0;
    var p: IProvider := new Impl();
    for var j in 0 ..< 200 do
      for var x in Make(p) do t := t + x; end;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
