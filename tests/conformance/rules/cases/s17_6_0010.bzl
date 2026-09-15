//! case: s17_6_0010
//! doc: spec
//! section: 17.6
//! title: ++ on a property with a setter increments through the accessors
//! rule: "The operand must be an assignable lvalue (a local variable, a
//!   parameter, a writable field, an indexer slot, or a property with a
//!   setter) ... `x++` is equivalent to `x := x + 1`."
//! cites: 17.6, 23.3
//! expect: run
//! stdout: 1
module Conf;

public class Gauge
  private _level: int := 0;

  public constructor()
  end constructor;

  public property Level: int
    get => _level;
    set => do
      _level := value;
    end;
  end Level;
end Gauge;

public class Program
  public static method Main()
    using var g := new Gauge();
    g.Level++;
    Console.WriteLine(g.Level);
  end Main;
end Program;
