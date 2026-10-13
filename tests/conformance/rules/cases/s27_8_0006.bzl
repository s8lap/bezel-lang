//! case: s27_8_0006
//! doc: spec
//! section: 27.8
//! title: an alias to a generic instance spelled on the base index parameter still overrides the indexer
//! rule: A type alias is a pure synonym, so an index parameter declared AI (type AI := Array<int>) and one declared Array<int> denote the same type, and the override dispatches through a base-typed receiver.
//! cites: 27.8
//! expect: run
//! stdout: 2
//! issue: 6041
module Conf;

type AI := Array<int>;

public class B
  public virtual property [a: AI]: int
    get => 1;
  end;
end B;

public class Dv: B
  public override property [a: Array<int>]: int
    get => 2;
  end;
end Dv;

public class M
  public static method Main()
    var b: B := new Dv();
    Console.WriteLine(b[new Array<int>()].ToString());
  end Main;
end M;
