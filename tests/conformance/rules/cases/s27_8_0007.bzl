//! case: s27_8_0007
//! doc: spec
//! section: 27.8
//! title: with no alias anywhere an indexer override dispatches through a base-typed receiver
//! rule: A type alias is not a separate kind of type; the control here has no alias, so it shows the override dispatches through a base-typed receiver when both index parameters are spelled Exception. The alias rows are read against this control.
//! cites: 27.8
//! expect: run
//! stdout: 2
//! issue: 6041
module Conf;

public class B
  public virtual property [e: Exception]: int
    get => 1;
  end;
end B;

public class Dv: B
  public override property [e: Exception]: int
    get => 2;
  end;
end Dv;

public class M
  public static method Main()
    var b: B := new Dv();
    Console.WriteLine(b[new Exception()].ToString());
  end Main;
end M;
