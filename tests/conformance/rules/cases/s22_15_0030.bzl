//! case: s22_15_0030
//! doc: spec
//! section: 22.15
//! title: the throw operand may be a user trivial-dispose exception with a pure constructor, in both spellings
//! rule: Inside a pure body, `new E(args)` is admitted only as the immediate
//!   operand of a `throw` statement or `throw` expression, and only when
//! cites: 22.15
//! expect: run
//! issue: 7453
//! output: caught A
//!   caught B
//!   5
module Conf;

public class MyError: Exception
  public pure constructor(message: string)
    base(message);
  end constructor;
end MyError;

public class Ops
  public static pure method A(s: string?): string => s ?? throw new MyError("null");
  public static pure method B(x: int): int
    if x < 0 then
      throw new MyError("neg");
    end;
    return x;
  end B;
end Ops;

public class Program
  public static method Main()
    try
      _ := Ops.A(null);
    catch
      var e: MyError => Console.WriteLine("caught A"),
    end;
    try
      _ := Ops.B(-1);
    catch
      var e: MyError => Console.WriteLine("caught B"),
    end;
    Console.WriteLine(Ops.B(5));
  end Main;
end Program;
