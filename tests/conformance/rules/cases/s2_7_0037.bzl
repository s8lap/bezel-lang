//! case: s2_7_0037
//! doc: spec
//! section: 2.7
//! title: a user type named System shadows the module prefix
//! rule: A prefix matching no import is an error at the leftmost segment
//!   (v0.338, §2.7 rule 2).
//! cites: 2.7
//! expect: reject
//! diagnostic: has no nested type named `Exception`
module Conf;

public class System
  public constructor() end constructor;
end System;
public class B
  public constructor() end constructor;
  public method Take(e: System.Exception): int => 1;
end B;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
