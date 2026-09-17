//! case: s20_0007
//! doc: spec
//! section: 20
//! title: sibling classes with no reciprocal conversion have no common type — no LUB search
//! rule: "If neither converts into the other, the expression is a compile error
//!   directing to an explicit conversion or upcast — there is no sibling-class
//!   least-upper-bound search." The smallest legal edit of s20_0006 — replacing
//!   the `Animal` branch with the sibling class `Cat` — loses the common type.
//! cites: 20, 5.1
//! expect: reject
//! diagnostic: explicit conversion
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public class Cat: Animal
  public constructor()
  end constructor;
end Cat;

public class Program
  public static method Main()
    var preferDog := true;
    using var pet := (if preferDog then new Dog() else new Cat());
  end Main;
end Program;
