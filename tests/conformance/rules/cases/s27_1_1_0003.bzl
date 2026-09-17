//! case: s27_1_1_0003
//! doc: spec
//! section: 27.1.1
//! title: a narrower-return function value is usable where a wider return is expected
//! rule: The return slot, when of class or interface type, is covariant — the
//!   value's return type must implicitly reference-convert to the target's; a
//!   `() -> Dog` value is usable as `() -> Animal`.
//! cites: 27.1.1, 5.1, 28.3
//! expect: accept
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public class Kennel
  private _resident: Dog;

  public constructor(resident: Dog)
    _resident := resident;
  end constructor;

  public method Resident(): Dog
    return _resident;
  end Resident;
end Kennel;

public class Program
  public method Demo(k: Kennel)
    var getDog: () -> Dog := k.Resident;
    var getAnimal: () -> Animal := getDog;
  end Demo;
end Program;
