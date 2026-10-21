//! case: s27_3_0055
//! doc: spec
//! section: 27.3
//! title: a procedure-bodied lambda is rejected for the sole static method value-returning function parameter
//! rule: "a procedure-bodied lambda does not convert to a value-returning function type, nor the reverse" ([§27.3]); the method's only overload takes `() -> int`, so the procedure lambda has no admissible slot.
//! cites: 27.3, 22.6
//! expect: reject
//! diagnostic: procedure-bodied
module Conf;

public class Program
  public static method Over(f: () -> int): string => "int";

  public static method Main()
    Console.WriteLine(Over(lambda() => do Console.WriteLine("x"); end));
  end Main;
end Program;
