//! case: s2_3_0044
//! doc: spec
//! section: 2.3
//! title: a qualified CancellationToken without import is rejected
//! rule: `import` makes the public names of the named module visible in
//!   the current file (v0.338, §2.2).
//! cites: 2.3, 2.2
//! expect: reject
//! diagnostic: unresolved type name `System`
module Conf;

public class B
  public constructor() end constructor;
  public method Take(t: System.Threading.CancellationToken): int => 1;
end B;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
