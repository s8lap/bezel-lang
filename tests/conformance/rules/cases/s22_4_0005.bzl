//! case: s22_4_0005
//! doc: spec
//! section: 22.4
//! title: out using var against a plain out parameter is rejected
//! rule: "A caller cannot write `out using var` against a plain `out`
//!   parameter (the result would have no owned source)."
//! cites: 22.4
//! expect: reject
//! diagnostic: no owned source
module Conf;

public class Parser
  public static method TryParse(s: string, out n: int): bool
    n := 0;
    return false;
  end TryParse;
end Parser;

public class Program
  public static method Main()
    if Parser.TryParse("x", out using var n) then
    end if;
  end Main;
end Program;
