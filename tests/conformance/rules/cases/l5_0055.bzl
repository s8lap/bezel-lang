//! case: l5_0055
//! doc: stdlib
//! section: 5
//! title: A parameter named Count of function type is called inside the method
//! rule: Only an instance member blocks an extension (spec 26.5 rule 1); a call whose callee is a parameter name is a call of that parameter.
//! cites: 22, 26.5
//! expect: run
//! output: 3
module Conf;

public class H
  public static method F(Count: (int) -> int): int => Count(3);
end H;

public class Program
  public static method Main()
    Console.WriteLine(H.F(lambda(x) => x));
  end Main;
end Program;
