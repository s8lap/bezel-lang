//! case: s27_1_1_0004
//! doc: spec
//! section: 27.1.1
//! title: a wider-return function value is not usable where a narrower return is expected
//! rule: The return slot, when of class or interface type, is covariant — the
//!   value's return type must implicitly reference-convert to the target's; a
//!   `() -> Animal` value is not usable as `() -> Dog`.
//! cites: 27.1.1, 5.1, 28.3
//! expect: reject
//! diagnostic: return type
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public class Shelter
  private _resident: Animal;

  public constructor(resident: Animal)
    _resident := resident;
  end constructor;

  public method Resident(): Animal
    return _resident;
  end Resident;
end Shelter;

public class Program
  public method Demo(s: Shelter)
    var getAnimal: () -> Animal := s.Resident;
    var getDog: () -> Dog := getAnimal;
  end Demo;
end Program;
