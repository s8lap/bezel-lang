//! case: s22_4_0007
//! doc: spec
//! section: 22.4
//! title: a short-circuited and-guard definitely assigns the inline out variable
//! rule: "The analysis is required to honor the standard short-circuit
//!   facts (`A and Call(out var x)` assigns `x` when the whole expression
//!   is `true`) ... so the idiomatic guarded read stays accepted."
//! cites: 22.4, 7.9
//! expect: run
//! output: forty-two
module Conf;

public class Parser
  public static method TryLabel(input: string, out label: string): bool
    if input = "42" then
      label := "forty-two";
      return true;
    end if;
    label := "";
    return false;
  end TryLabel;
end Parser;

public class Program
  public static method Main()
    var flag := true;
    if flag and Parser.TryLabel("42", out var label) then
      Console.WriteLine(label);
    end if;
  end Main;
end Program;
