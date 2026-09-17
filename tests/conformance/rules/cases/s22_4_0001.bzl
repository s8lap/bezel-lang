//! case: s22_4_0001
//! doc: spec
//! section: 22.4
//! title: out var name declares a new variable inline, typed from the parameter
//! rule: "`out var name` declares a new variable inline. Its type is
//!   inferred from the method's `out` parameter declaration."
//! cites: 22.4
//! expect: run
//! stdout: 42
module Conf;

public class Parser
  public static method TryParse(s: string, out n: int): bool
    if s = "42" then
      n := 42;
      return true;
    end if;
    n := 0;
    return false;
  end TryParse;
end Parser;

public class Program
  public static method Main()
    var success := Parser.TryParse("42", out var num);
    Console.WriteLine(num);
  end Main;
end Program;
