//! case: s31_3_0001
//! doc: spec
//! section: 31.3
//! title: a method that may throw needs no exception declaration
//! rule: Methods do not declare which exceptions they may raise. The compiler
//!   does not enforce that callers handle particular exception types. Any
//!   exception may propagate from any method call.
//! cites: 31.3
//! expect: run
//! stdout: must be positive
module Conf;

public class Program
  public static method Validate(amount: int)
    if amount <= 0 then
      throw new ArgumentException("must be positive", nameof(amount));
    end if;
  end Validate;

  public static method Main()
    try
      Validate(-1);
    catch
      var ex: ArgumentException => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
