//! case: s26_6_0001
//! doc: spec
//! section: 26.6
//! title: multiple extension blocks on the same type are merged
//! rule: Multiple `extension` blocks targeting the same type may appear
//!   across files in the same module. The compiler merges them.
//! cites: 26.6
//! expect: run
//! stdout: ab
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public extension Widget
  public method Tag(): string => "a";
end;

public extension Widget
  public method Label(): string => "b";
end;

public class Program
  public static method Main()
    using var w := new Widget();
    Console.WriteLine(w.Tag() + w.Label());
  end Main;
end Program;
