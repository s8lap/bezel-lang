//! case: s7_4_0061
//! doc: spec
//! section: 7.4
//! title: a break leaving a try runs the finally's null store before the read after the loop
//! rule: "`finally` runs whether the protected block completed normally, was exited by an exception
//!   (caught or uncaught), or was exited by `return` — or by a `break` or `continue` targeting an
//!   enclosing loop." ([§31.2]). "the variable is reassigned from a nullable-typed expression"
//!   ([§7.4]): on the break path the finally's null store runs before the read after the loop.
//! cites: 7.4, 31.2
//! expect: reject
//! diagnostic: nullable
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
        s := null;
      end try;
      s := "y";
      i := i + 1;
    end while;
    Console.WriteLine(s.Length);
  end Main;
end Program;
