//! case: s2_7_0035
//! doc: spec
//! section: 2.7
//! title: an unknown qualifier before a type name is still rejected
//! rule: A prefix matching no import is an error at the leftmost segment
//!   (v0.338, §2.7 rule 2).
//! cites: 2.7
//! expect: reject
//! diagnostic: unresolved type name `Foo`
module Conf;

public class B
  public constructor() end constructor;
  public method Take(e: Foo.Exception): int => 1;
end B;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
