//! case: s28_9_9802
//! doc: spec
//! section: 28.9
//! title: A call through the same property via a second reference to the receiver, 200x
//! rule: A property or indexer **read** is a lend for every declared type
//! cites: 28.9, 27.5, 28.3
//! expect: run
//! output: 399
//! notes: Pins #9163 (pick 92522a9bc): a call through a function-typed property whose getter reads a field
//!   must hold its own reference for the call, because the callee closure replaces the field it came
//!   from while running. Extended shape: the property is read through a local alias of the receiver.
module Conf;

public class Hx
  private _q: (Hx) -> int := lambda(x: Hx) => 0;
  public property P: (Hx) -> int => _q;
  public constructor() end constructor;
  public method Fill(k: int) _q := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill;
  public method Drop() _q := lambda(x: Hx) => 0; end Drop;
  public method Run(): int
    var h2 := this; return h2.P(h2);
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
