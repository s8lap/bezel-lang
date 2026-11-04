//! case: s22_15_0802
//! doc: spec
//! section: 22.15
//! title: a pure-declared constructor at a throw expression (?? throw new E(...)) in a pure method is fine
//! rule: Inside a pure body, `new E(args)` is admitted only as the immediate operand of a `throw` statement or `throw` expression, and only when **(a)** the invoked constructor of `E` is `pure`-declared
//! cites: 22.15, 31.4
//! expect: run
//! stdout: a
//! issue: 8134
module Conf;

public class MyError: Exception
  public pure constructor(message: string, code: int := 0) base(message);
  end constructor;
end MyError;

public class Program
  public static pure method Run(s: string?): string
    return s ?? throw new MyError("none");
  end Run;
  public static method Main()
    Console.WriteLine(Run("a"));
  end Main;
end Program;
