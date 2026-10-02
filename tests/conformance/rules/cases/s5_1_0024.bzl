//! case: s5_1_0024
//! doc: spec
//! section: 5.1
//! title: an unrelated class reference does not implicitly convert to another unrelated class
//! rule: Unrelated classes have no inheritance relationship; implicit reference
//!   conversions are only those listed in §5.1 (class to base class, transitively).
//!   This is NOT a downcast — there is no base/derived relationship at all —
//!   so the generic "not implicitly convertible" message applies.
//! cites: 5.1
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Vehicle
  public constructor()
  end constructor;
end Vehicle;

public class Program
  public static method Convert(a: Animal)
    var v: Vehicle := a;
  end Convert;
end Program;
