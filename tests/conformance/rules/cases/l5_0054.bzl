//! case: l5_0054
//! doc: stdlib
//! section: 5
//! title: A local variable named Count of function type is called, not resolved as the LINQ operator
//! rule: Only an instance member blocks an extension (spec 26.5 rule 1); a call whose callee is a plain local name is a call of that local (spec 22), with no receiver to block or extend.
//! cites: 22, 26.5
//! expect: run
//! output: 5
module Conf;

public class Program
  public static method Main()
    var Count := lambda(x: int) => x + 1;
    Console.WriteLine(Count(4));
  end Main;
end Program;
