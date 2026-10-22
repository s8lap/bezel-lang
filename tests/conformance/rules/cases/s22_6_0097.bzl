//! case: s22_6_0097
//! doc: spec
//! section: 22.6
//! title: an exact argument type beats a covariant conversion in overload ranking
//! rule: "the better candidate requires *no* implicit conversion (the argument's static type matches the parameter type exactly) and the other requires an implicit conversion" ([§22.6]). The argument is an IEnumerable<Dog>, exact for the IEnumerable<Dog> overload and a covariant conversion for the IEnumerable<Animal> overload, so the Dog overload wins.
//! cites: 12.9, 5.1
//! expect: run
//! output: 2
module Conf;
public class Animal
  public constructor()
  end constructor;
end Animal;
public class Dog: Animal
  public constructor()
  end constructor;
end Dog;
public static class S
  public static method F(x: IEnumerable<Animal>): int => 1;
  public static method F(x: IEnumerable<Dog>): int => 2;
end S;
public class Program
  public static method Main(): int
    using var arr := new Array<Dog>();
    var v: IEnumerable<Dog> := arr;
    Console.WriteLine(S.F(v));
    return 0;
  end Main;
end Program;
