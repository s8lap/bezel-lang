//! case: s22_15_0801
//! doc: spec
//! section: 22.15
//! title: a pure constructor with a defaulted parameter, invoked without it, is admitted at a throw operand
//! rule: Inside a pure body, `new E(args)` is admitted only as the immediate operand of a `throw` statement or `throw` expression, and only when **(a)** the invoked constructor of `E` is `pure`-declared
//! cites: 22.15, 31.4
//! expect: run
//! stdout: caught neg
//! issue: 8134
//! notes: the only constructor is pure-declared; its base(message) target is a built-in exception (message) form, pure per 31.4.
module Conf;

public class MyError: Exception
  public pure constructor(message: string, code: int := 0) base(message);
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
