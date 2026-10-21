//! case: s27_3_0057
//! doc: spec
//! section: 27.3
//! title: a procedure-bodied lambda is rejected when every same-name overload takes a value-returning function
//! rule: "a procedure-bodied lambda does not convert to a value-returning function type, nor the reverse" ([§27.3]); both same-name overloads take a value-returning function, so no slot admits the procedure lambda.
//! cites: 27.3, 22.6
//! expect: reject
//! diagnostic: procedure-bodied
//! issue: 6268
module Conf;

public class Program
  public static method Over(f: () -> int): string => "int";
  public static method Over(f: () -> string): string => "str";

  public static method Main()
    Console.WriteLine(Over(lambda() => do Console.WriteLine("x"); end));
  end Main;
end Program;
