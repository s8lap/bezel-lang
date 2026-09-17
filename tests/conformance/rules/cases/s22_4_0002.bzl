//! case: s22_4_0002
//! doc: spec
//! section: 22.4
//! title: out var underscore is a compile error
//! rule: "`out var _` is a compile error — it would declare nothing; write
//!   `out _`, which already means exactly this."
//! cites: 22.4, 17.13
//! expect: reject
//! diagnostic: declare nothing
module Conf;

public class Parser
  public static method TryParse(s: string, out n: int): bool
    n := 0;
    return false;
  end TryParse;
end Parser;

public class Program
  public static method Main()
    var success := Parser.TryParse("x", out var _);
  end Main;
end Program;
