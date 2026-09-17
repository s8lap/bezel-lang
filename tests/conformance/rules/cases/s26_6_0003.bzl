//! case: s26_6_0003
//! doc: spec
//! section: 26.6
//! title: merged blocks may contribute distinct overloads of the same name
//! rule: Multiple `extension` blocks targeting the same type are merged;
//!   duplicate *signatures* are a compile error, but two blocks
//!   contributing different overloads of one name are not duplicates.
//! cites: 26.6
//! expect: run
//! stdout: aa1
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public extension Widget
  public method Tag(): string => "a";
end;

public extension Widget
  public method Tag(n: int): string => "a" + n.ToString();
end;

public class Program
  public static method Main()
    using var w := new Widget();
    Console.WriteLine(w.Tag() + w.Tag(1));
  end Main;
end Program;
