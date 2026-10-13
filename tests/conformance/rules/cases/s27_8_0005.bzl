//! case: s27_8_0005
//! doc: spec
//! section: 27.8
//! title: an alias of an alias on the base index parameter still overrides the indexer
//! rule: A type alias is not a separate kind of type; identity, equality and method dispatch all see through the alias, so the override's index parameter and the base's index parameter denote the same type and the override dispatches. Here the base spells [e: EB] and the override spells [e: Exception].
//! cites: 27.8
//! expect: run
//! stdout: 2
//! issue: 6041
module Conf;

type EA := Exception;
type EB := EA;

public class B
  public virtual property [e: EB]: int
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
