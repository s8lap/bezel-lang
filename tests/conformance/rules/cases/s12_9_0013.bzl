//! case: s12_9_0013
//! doc: spec
//! section: 12.9
//! title: an invariant interface parameter is applicable only at the instantiation the argument implements
//! rule: "Type parameters without `out` or `in` are invariant." ([§12.9]). IBox has no variance marker, DogBox implements IBox<Dog> and not IBox<Animal>, so only the IBox<Dog> overload is applicable.
//! cites: 22.6, 14.5
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
  public static method F(x: IBox<Animal>): int => 1;
  public static method F(x: IBox<Dog>): int => 2;
end S;
public class Program
  public static method Main(): int
    using var a := new DogBox();
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
end Program;
