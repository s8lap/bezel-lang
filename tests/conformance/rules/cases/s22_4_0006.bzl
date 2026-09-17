//! case: s22_4_0006
//! doc: spec
//! section: 22.4
//! title: an explicit type annotation on the inline out form is not permitted
//! rule: An explicit type annotation is not permitted in the inline `out`
//!   form; the type is inferred from the method's `out` parameter
//!   declaration.
//! cites: 22.4
//! expect: reject
//! diagnostic: not permitted
module Conf;

public class Parser
  public static method TryParse(s: string, out n: int): bool
    n := 0;
    return false;
  end TryParse;
end Parser;

public class Program
  public static method Main()
    var ok := Parser.TryParse("x", out var n: int);
  end Main;
end Program;
