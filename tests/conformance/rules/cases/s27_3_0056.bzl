//! case: s27_3_0056
//! doc: spec
//! section: 27.3
//! title: a procedure-bodied lambda is rejected beside a same-name overload of a different arity
//! rule: "a procedure-bodied lambda does not convert to a value-returning function type, nor the reverse" ([§27.3]); the only overload applicable by arity takes `() -> int`, so the procedure lambda has no admissible slot.
//! cites: 27.3, 22.6
//! expect: reject
//! diagnostic: procedure-bodied
//! issue: 6268
module Conf;

public class Program
  public static method Over(f: () -> int): string => "int";
  public static method Over(f: () -> int, n: int): string => "int2";

  public static method Main()
    Console.WriteLine(Over(lambda() => do Console.WriteLine("x"); end));
  end Main;
end Program;
