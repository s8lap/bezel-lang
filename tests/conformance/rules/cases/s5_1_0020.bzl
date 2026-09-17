//! case: s5_1_0020
//! doc: spec
//! section: 5.1
//! title: a base class reference does not implicitly convert to a derived class
//! rule: Implicit conversions occur only as listed in §5.1; a downcast from a
//!   base to a derived class is not among the listed reference conversions.
//!   Twin of s5_1_0019: same pair, direction reversed.
//! cites: 5.1
//! expect: reject
//! diagnostic: only as listed
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public class Program
  public static method Convert(a: Animal)
    var d: Dog := a;
  end Convert;
end Program;
