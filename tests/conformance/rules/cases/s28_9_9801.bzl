//! case: s28_9_9801
//! doc: spec
//! section: 28.9
//! title: A call through an expression-bodied function-typed property whose getter reads a field, 200x
//! rule: A property or indexer **read** is a lend for every declared type
//! cites: 28.9, 27.5, 28.3
//! expect: run
//! output: 399
//! notes: Pins #9163 (pick 92522a9bc): a call through a function-typed property whose getter reads a field
//!   must hold its own reference for the call, because the callee closure replaces the field it came
//!   from while running. Base shape: the getter is the plain field read.
module Conf;

public class Hx
  private _q: (Hx) -> int := lambda(x: Hx) => 0;
  public property P: (Hx) -> int => _q;
  public constructor() end constructor;
  public method Fill(k: int) _q := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill;
  public method Drop() _q := lambda(x: Hx) => 0; end Drop;
  public method Run(): int
    return P(this);
  end Run;
end Hx;

public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx(); h.Fill(i mod 3); t := t + h.Run();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
