//! case: s22_4_0007
//! doc: spec
//! section: 22.4
//! title: a short-circuited and-guard definitely assigns the inline out variable
//! rule: "`A and Call(out var x)` assigns `x` when the whole expression is
//!   `true`" — the idiomatic guarded read stays accepted.
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
    var flag := true;
    if flag and Parser.TryParse("42", out var n) then
      Console.WriteLine(n);
    end if;
  end Main;
end Program;
