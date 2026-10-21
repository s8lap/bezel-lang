//! case: s7_4_0058
//! doc: spec
//! section: 7.4
//! title: a finally that stores a non-null string leaves the read after the try narrowed
//! rule: "`finally` runs whether the protected block completed normally, was exited by an exception
//!   (caught or uncaught), or was exited by `return` — or by a `break` or `continue` targeting an
//!   enclosing loop." ([§31.2]). "after `x := expr;` where `expr`'s static type is non-nullable"
//!   ([§7.4]) "`x` is narrowed until invalidated" ([§7.4]): the finally stores a non-nullable value
//!   on every path, so the read is narrowed.
//! cites: 7.4, 31.2
//! expect: run
//! stdout: 3
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
      s := "zzz";
    end try;
    Console.WriteLine(s.Length);
  end Main;
end Program;
