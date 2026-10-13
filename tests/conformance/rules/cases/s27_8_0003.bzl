//! case: s27_8_0003
//! doc: spec
//! section: 27.8
//! title: an indexer override whose index parameter is spelled through a type alias overrides the base indexer
//! rule: A type alias is not a separate kind of type; identity, equality and method dispatch all see through the alias. An override whose index parameter is spelled EA (an alias of Exception) overrides the base indexer declared over Exception, so a read through a base-typed receiver dispatches the override.
//! cites: 27.8
//! expect: run
//! stdout: 2
//! issue: 6041
module Conf;

type EA := Exception;

public class B
  public virtual property [e: EA]: int
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
