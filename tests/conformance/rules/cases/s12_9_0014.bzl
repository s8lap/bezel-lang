//! case: s12_9_0014
//! doc: spec
//! section: 12.9
//! title: a contravariant interface argument converts to the narrower instantiation
//! rule: "Generic interface type parameters may be marked `out` (covariant) or `in` (contravariant)" ([§12.9]). ISink<in T> reverses the direction: AnimalSink implements ISink<Animal>, which converts to ISink<Dog>, so the ISink<Dog> overload is the only applicable one, and the assignment to ISink<Dog> prints the value the Animal sink returns.
//! cites: 22.6
//! expect: run
//! output: 2
//!   5
module Conf;
public class Animal
  public constructor()
  end constructor;
end Animal;
public class Dog: Animal
  public constructor()
  end constructor;
end Dog;
public interface ISink<in T>
  method Put(x: T): int;
end ISink;
public class AnimalSink: ISink<Animal>
  public constructor()
  end constructor;
  public method Put(x: Animal): int => 5;
end AnimalSink;
public static class S
  public static method F(x: ISink<string>): int => 1;
  public static method F(x: ISink<Dog>): int => 2;
end S;
public class Program
  public static method Main(): int
    using var a := new AnimalSink();
    using var d := new Dog();
    Console.WriteLine(S.F(a));
    var s: ISink<Dog> := a;
    Console.WriteLine(s.Put(d));
    return 0;
  end Main;
end Program;
