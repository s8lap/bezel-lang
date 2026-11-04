//! case: s22_15_0806
//! doc: spec
//! section: 22.15
//! title: a default that makes the impure constructor the invoked one makes the throw operand rejected
//! rule: Inside a pure body, `new E(args)` is admitted only as the immediate operand of a `throw` statement or `throw` expression, and only when **(a)** the invoked constructor of `E` is `pure`-declared
//! cites: 22.15, 22.5
//! expect: reject
//! diagnostic: pure
//! issue: 8134
//! notes: new MyError("neg") applies only to the second constructor (the pure one needs two arguments).
module Conf;

public class MyError: Exception
  public pure constructor(message: string, code: int) base(message);
  end constructor;
  public constructor(message: string, tag: string := "d") base(message);
    Console.WriteLine("side effect " + tag);
  end constructor;
end MyError;

public class Program
  public static pure method Run(x: int): int
    if x < 0 then
      throw new MyError("neg");
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
