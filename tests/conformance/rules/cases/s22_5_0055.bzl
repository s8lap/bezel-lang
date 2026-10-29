//! case: s22_5_0055
//! doc: spec
//! section: 22.5
//! title: a named argument that names no parameter of the DateTime constructor is rejected
//! rule: Each supplied argument binds **exactly one** parameter and each parameter is bound by **at most one** supplied argument
//! cites: 22.5
//! expect: reject
//! diagnostic: has no parameter named
module Conf;

public class Program
  public static method Build(): DateTime
    return new DateTime(2020, 1, 2, 3, 4, 5, bogus := 6);
  end Build;
end Program;
