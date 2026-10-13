//! case: s12_1_0040
//! doc: spec
//! section: 12.1
//! title: widening to T? does not admit a changed built-in generic argument
//! rule: An override may declare a parameter as `T?` where the overridden
//!   member declares `T`; visibility, return type and parameter types
//!   otherwise match the base member exactly (v0.338, §12.1).
//! cites: 12.1
//! expect: reject
//! diagnostic: does not match the base member's parameter types
module Conf;

public class B
  public constructor() end constructor;
  public virtual method Log(e: Array<Exception>): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: Array<ArgumentException>?): int => 2;
end C;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
