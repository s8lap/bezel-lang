//! case: s22_15_0804
//! doc: spec
//! section: 22.15
//! title: a throw operand whose only constructor is not pure-declared (it prints) is rejected
//! rule: Inside a pure body, `new E(args)` is admitted only as the immediate operand of a `throw` statement or `throw` expression, and only when **(a)** the invoked constructor of `E` is `pure`-declared
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
//! issue: 8134
//! notes: twin s22_15_0801: the constructor is pure-declared there.
module Conf;

public class MyError: Exception
  public constructor(message: string) base(message);
    Console.WriteLine("side effect");
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
