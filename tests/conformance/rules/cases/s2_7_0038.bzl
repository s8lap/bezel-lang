//! case: s2_7_0038
//! doc: spec
//! section: 2.7
//! title: a local named like a collection type is a value, so a dotted access on it is member access
//! rule: If it resolves to a *value* (a local, field, or property), the chain is
//!   member access.
//! cites: 2.7
//! expect: run
//! stdout: 5
module Conf;

public class Box
  public method Get(): int
    return 5;
  end Get;
end Box;

public class Program
  public static method Main()
    var Array := new Box();
    Console.WriteLine($"{Array.Get()}");
  end Main;
end Program;
