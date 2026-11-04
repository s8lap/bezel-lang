//! case: s26_5_0059
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on a generic Task<T> parameter is rejected
//! rule: Where neither rule 1 nor this search supplies the name, nothing
//!   supplies it: the access names no member and is a compile error at the
//!   name, the diagnostic naming the receiver's static type (§26.5 rule 2,
//!   v0.355).
//! cites: 26.5, 30.3
//! expect: reject
//! diagnostic: Bogus
module Conf;

public class Program
  public static method Poke<T>(t: Task<T>)
    t.Bogus();
  end Poke;
end Program;
