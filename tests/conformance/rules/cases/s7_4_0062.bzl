//! case: s7_4_0062
//! doc: spec
//! section: 7.4
//! title: a break leaving a try reads a narrowed string when the finally stores a non-null value
//! rule: "`finally` runs whether the protected block completed normally, was exited by an exception
//!   (caught or uncaught), or was exited by `return` — or by a `break` or `continue` targeting an
//!   enclosing loop." ([§31.2]). "after `x := expr;` where `expr`'s static type is non-nullable"
//!   ([§7.4]) "`x` is narrowed until invalidated" ([§7.4]): every path to the read after the loop
//!   ends with a non-nullable finally store.
//! cites: 7.4, 31.2
//! expect: run
//! stdout: 3
//! issue: 6682
module Conf;

public class Program
  public static method Main()
    var s: string? := "x";
    var i := 0;
    while i < 2 do
      try
        if i = 1 then break; end;
      finally
        s := "zzz";
      end try;
      s := "y";
      i := i + 1;
    end while;
    Console.WriteLine(s.Length);
  end Main;
end Program;
