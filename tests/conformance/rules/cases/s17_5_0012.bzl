//! case: s17_5_0012
//! doc: spec
//! section: 17.5
//! title: a Dog left operand is admitted into an Array of its base class and finds its own element
//! rule: "the left operand is admitted exactly when it is admissible as that
//!   call's argument under the ordinary conversion rules ([§5.1])", and a
//!   class converts implicitly to each base class ([§5.1]). A class's default
//!   `Equals` is reference equality ([§8.3]), so the element found is the
//!   same object.
//! cites: 17.5, 5.1, 8.3
//! expect: run
//! stdout: True
module Conf;

public class Animal
end Animal;

public class Dog: Animal
end Dog;

public class Program
  public static method Main()
    var d := new Dog();
    var a: Array<Animal> := [d];
    var found := d in a;
    Console.WriteLine(found);
  end Main;
end Program;
