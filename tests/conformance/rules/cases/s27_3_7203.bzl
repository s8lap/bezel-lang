//! case: s27_3_7203
//! doc: spec
//! section: 27.3
//! title: a procedure-bodied lambda argument at a generic class member's
//!   value slot is rejected
//! rule: a procedure-bodied lambda does not convert to a value-returning
//!   function type, nor the reverse
//! cites: 27.3, 14.5
//! expect: reject
//! diagnostic: procedure-bodied
module Conf;

public class Box<T>
  public method E(f: () -> T): T
    return f();
  end E;
end Box;

public class Program
  public static method Main()
    var b: Box<int> := new Box<int>();
    Console.WriteLine(b.E(lambda() => do Console.Write("p"); end));
  end Main;
end Program;
