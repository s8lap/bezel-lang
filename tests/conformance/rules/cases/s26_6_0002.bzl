//! case: s26_6_0002
//! doc: spec
//! section: 26.6
//! title: duplicate signatures across merged blocks are a compile error
//! rule: Multiple `extension` blocks targeting the same type may appear;
//!   the compiler merges them. Duplicate signatures are a compile error.
//! cites: 26.6
//! expect: reject
//! diagnostic: duplicate
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public extension Widget
  public method Tag(): string => "a";
end;

public extension Widget
  public method Tag(): string => "b";
end;

public class Program
  public static method Main()
  end Main;
end Program;
