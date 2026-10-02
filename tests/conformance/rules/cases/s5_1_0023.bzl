//! case: s5_1_0023
//! doc: spec
//! section: 5.1
//! title: a base class reference does not implicitly convert to a derived class (downcast)
//! rule: A base-to-derived (downcast) conversion is NOT implicit per §5.1.
//!   The diagnostic wording must identify this as a downcast and cite §5.2
//!   for the explicit cast syntax. The target type name in the diagnostic
//!   must be the real target type (Dog), not a hardcoded placeholder.
//! cites: 5.1
//! expect: reject
//! diagnostic: downcast
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
