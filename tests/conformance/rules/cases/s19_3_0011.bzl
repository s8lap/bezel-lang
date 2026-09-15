//! case: s19_3_0011
//! doc: spec
//! section: 19.3
//! title: an unconstrained var pattern matches any value and binds it
//! rule: "**Var patterns:** `var t`. Matches any value, binds it."
//! cites: 19.3
//! expect: run
//! stdout: got 42
module Conf;

public class Program
  public static method Main()
    var n := 42;
    case n of
      var t => Console.WriteLine($"got {t}")
    end case;
  end Main;
end Program;
