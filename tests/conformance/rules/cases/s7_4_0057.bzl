//! case: s7_4_0057
//! doc: spec
//! section: 7.4
//! title: a finally that stores null makes the read after the try nullable
//! rule: "`finally` runs whether the protected block completed normally, was exited by an exception
//!   (caught or uncaught), or was exited by `return` — or by a `break` or `continue` targeting an
//!   enclosing loop." ([§31.2]). "the variable is reassigned from a nullable-typed expression"
//!   ([§7.4]): the finally's null store is on the only path to the read.
//! cites: 7.4, 31.2
//! expect: reject
//! diagnostic: nullable
//! issue: 6673
module Conf;

public class Program
  public static method Boom(f: bool)
    if f then throw new InvalidOperationException("x"); end;
  end Boom;

  public static method Main()
    var s: string? := "x";
    try
      Boom(false);
    finally
      s := null;
    end try;
    Console.WriteLine(s.Length);
  end Main;
end Program;
