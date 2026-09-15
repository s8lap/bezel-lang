//! case: s17_4_0003
//! doc: spec
//! section: 17.4
//! title: not (x is T) has the same truth value as x is not T
//! rule: "`not (x is T)` is a different expression (a boolean negation of a
//!   relational test) with the same truth value [as `x is not T`], but no
//!   narrowing in the negated branch."
//! cites: 17.4
//! expect: run
//! stdout: True
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
  public static method Main()
    using var a := new Animal();
    var viaIsNot := a is not Dog;
    var viaNotIs := not (a is Dog);
    Console.WriteLine(viaIsNot = viaNotIs);
  end Main;
end Program;
