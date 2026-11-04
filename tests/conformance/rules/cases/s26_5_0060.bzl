//! case: s26_5_0060
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on a task call result is rejected
//! rule: Where neither rule 1 nor this search supplies the name, nothing
//!   supplies it: the access names no member and is a compile error at the
//!   name, the diagnostic naming the receiver's static type.
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
