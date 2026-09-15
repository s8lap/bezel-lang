//! case: l4_12_0002
//! doc: stdlib
//! section: 4.12
//! title: IEnumerable<T> is covariant, admitting an Array<Derived> where IEnumerable<Base> is expected
//! rule: IEnumerable<T> is covariant in T.
//! cites: 4.12
//! expect: run
//! stdout: 1
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public static class Helpers
  public static method CountAnimals(source: IEnumerable<Animal>): int
    return source.Count();
  end CountAnimals;
end Helpers;

public class Program
  public static method Main()
    using var dogs: Array<Dog> := [new Dog()];
    Console.WriteLine(Helpers.CountAnimals(dogs));
  end Main;
end Program;
