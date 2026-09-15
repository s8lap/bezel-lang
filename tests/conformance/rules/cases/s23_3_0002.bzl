//! case: s23_3_0002
//! doc: spec
//! section: 23.3
//! title: a block-form setter body runs on assignment through the property
//! rule: A property's accessors are member bodies: a store through the property
//!   invokes the setter, and a read invokes the getter.
//! cites: 23.3
//! expect: run
//! stdout: 10
module Conf;

public class Doubler
  private _n: int := 0;

  public constructor()
  end constructor;

  public property Value: int
    get => _n;
    set => do _n := value * 2; end;
  end Value;
end Doubler;

public class Program
  public static method Main()
    using var d := new Doubler();
    d.Value := 5;
    Console.WriteLine(d.Value);
  end Main;
end Program;
