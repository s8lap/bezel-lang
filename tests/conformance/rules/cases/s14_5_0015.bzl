//! case: s14_5_0015
//! doc: spec
//! section: 14.5
//! title: a generic overload is applicable through the unique instantiation an argument implements
//! rule: "the unique instantiation of `G` that the argument's type implements or extends" ([§14.5]). DogBox implements IBox<Dog> and no other IBox instantiation, so the generic F binds T to Dog and is applicable, while F(IBox<string>) is not; the generic candidate is the only applicable one.
//! cites: 22.6
//! expect: run
//! output: 3
module Conf;
public class Dog
  public constructor()
  end constructor;
end Dog;
public interface IBox<T>
  method Get(): int;
end IBox;
public class Box<T>: IBox<T>
  public constructor()
  end constructor;
  public method Get(): int => 7;
end Box;
public class DogBox: Box<Dog>
  public constructor()
  end constructor;
end DogBox;
public static class S
  public static method F<T>(x: IBox<T>): int => 3;
  public static method F(x: IBox<string>): int => 2;
end S;
public class Program
  public static method Main(): int
    using var a := new DogBox();
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
end Program;
