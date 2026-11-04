//! case: s22_15_0807
//! doc: spec
//! section: 22.15
//! title: overload resolution by argument type that picks the impure constructor makes the throw rejected
//! rule: Inside a pure body, `new E(args)` is admitted only as the immediate operand of a `throw` statement or `throw` expression, and only when **(a)** the invoked constructor of `E` is `pure`-declared
//! cites: 22.15, 22.6
//! expect: reject
//! diagnostic: pure
//! issue: 8134
//! notes: the string second argument selects the impure (string, string) constructor.
module Conf;

public class MyError: Exception
  public pure constructor(message: string, code: int) base(message);
  end constructor;
  public constructor(message: string, tag: string) base(message);
    Console.WriteLine("side effect " + tag);
  end constructor;
end MyError;

public class Program
  public static pure method Run(x: int): int
    if x < 0 then
      throw new MyError("neg", "t");
    end;
    return x;
  end Run;
  public static method Main()
    try
      Console.WriteLine(Run(-1));
    catch var e: Exception => Console.WriteLine("caught " + e.Message);
    end;
  end Main;
end Program;
