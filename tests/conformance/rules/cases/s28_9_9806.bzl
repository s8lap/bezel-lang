//! case: s28_9_9806
//! doc: spec
//! section: 28.9
//! title: A call through an interface-typed receiver's function-typed property, 200x
//! rule: A property or indexer **read** is a lend for every declared type
//! cites: 28.9, 28.3
//! expect: run
//! output: 399
//! notes: Pins #9163 (pick 92522a9bc): a call through a function-typed property whose getter reads a field
//!   must hold its own reference for the call, because the callee closure replaces the field it came
//!   from while running. Control: the same call through the interface-typed view of the receiver.
module Conf;

public interface IHx
  readonly property P: (IHx) -> int;
  method Drop();
end IHx;

public class Hx: IHx
  private _q: (IHx) -> int := lambda(x: IHx) => 0;
  public property P: (IHx) -> int => _q;
  public constructor() end constructor;
  public method Drop() _q := lambda(x: IHx) => 0; end Drop;
  public method Fill(k: int) _q := lambda(x: IHx) => do x.Drop(); return k + 1; end; end Fill;
  public method Run(): int
    var ih: IHx := this;
    return ih.P(ih);
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
