//! case: s22_4_0003
//! doc: spec
//! section: 22.4
//! title: out underscore is the admitted discard spelling
//! rule: "`out var _` is a compile error — it would declare nothing; write
//!   `out _`, which already means exactly this."
//! cites: 22.4, 17.13
//! expect: accept
module Conf;

public class Parser
  public static method TryParse(s: string, out n: int): bool
    n := 0;
    return false;
  end TryParse;
end Parser;

public class Runner
  public static method Run()
    var success := Parser.TryParse("x", out _);
  end Main;
end Program;
