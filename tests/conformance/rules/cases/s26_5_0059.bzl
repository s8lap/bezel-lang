//! case: s26_5_0059
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on a generic Task<T> parameter is rejected
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules
//! cites: 26.5, 30.3
//! expect: reject
//! diagnostic: Bogus
module Conf;

public class Program
  public static method Poke<T>(t: Task<T>)
    t.Bogus();
  end Poke;
end Program;
