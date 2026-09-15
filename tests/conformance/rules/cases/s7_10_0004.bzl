//! case: s7_10_0004
//! doc: spec
//! section: 7.10
//! title: a non-nullable value type satisfies where T: value
//! rule: `where T: value` restricts T to non-nullable value types —
//!   primitives included.
//! cites: 14.2
//! expect: run
//! stdout: ok
module Conf;

public class Box<T> where T: value
  public constructor()
  end constructor;
end Box;

public class Program
  public static method Main()
    var b := new Box<int>();
    Console.WriteLine("ok");
  end Main;
end Program;
