//! case: s12_9_0012
//! doc: spec
//! section: 12.9
//! title: a covariant interface argument converts to the wider instantiation and not to string
//! rule: "Reference-typed T admits the full covariant chain — `IEnumerable<Dog>` is assignable to `IEnumerable<Animal>`" ([§12.9]). The same chain holds for an out-marked interface: DogBox implements IOBox<Dog>, which converts to IOBox<Animal> and not to IOBox<string>, so only the IOBox<Animal> overload is applicable.
//! cites: 22.6
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
public interface IOBox<out T>
  method Get(): int;
end IOBox;
public class Box<T>: IOBox<T>
  public constructor()
  end constructor;
  public method Get(): int => 7;
end Box;
public class DogBox: Box<Dog>
  public constructor()
  end constructor;
end DogBox;
public static class S
  public static method F(x: IOBox<string>): int => 1;
  public static method F(x: IOBox<Animal>): int => 2;
end S;
public class Program
  public static method Main(): int
    using var a := new DogBox();
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
end Program;
