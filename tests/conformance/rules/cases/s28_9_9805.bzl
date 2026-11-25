//! case: s28_9_9805
//! doc: spec
//! section: 28.9
//! title: A call through a property whose getter returns a fresh capture-free closure, 200x
//! rule: A property or indexer **read** is a lend for every declared type
//! cites: 28.9, 27.5
//! expect: run
//! output: 1400
//! notes: Pins #9163 (pick 92522a9bc): a call through a function-typed property whose getter reads a field
//!   must hold its own reference for the call, because the callee closure replaces the field it came
//!   from while running. Control: the getter returns a fresh closure each read; the call is 3 + 4 = 7
//!   per iteration, 1400 over 200.
module Conf;

public class Hx
  private _n: int := 3;
  public property P: (Hx) -> int => lambda(x: Hx) => x._n + 4;
  public constructor() end constructor;
  public method Run(): int
    return P(this);
  end Run;
end Hx;

public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx(); t := t + h.Run();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
