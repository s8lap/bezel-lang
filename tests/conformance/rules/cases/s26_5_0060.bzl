//! case: s26_5_0060
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on a task call result is rejected
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules
//! cites: 26.5, 30.3
//! expect: reject
//! diagnostic: Bogus
module Conf;

public class Program
  public static method G(): Task<int>
    return Task.FromResult(1);
  end G;

  public static method Main(): int
    G().Bogus();
    return 0;
  end Main;
end Program;
