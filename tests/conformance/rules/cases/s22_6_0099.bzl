//! case: s22_6_0099
//! doc: spec
//! section: 22.6
//! title: no applicable candidate for an invariant interface argument is a compile error
//! rule: "with no applicable candidate at all, the no-candidate error" ([§22.6]). DogBox implements only IBox<Dog>, which is neither IBox<Animal> nor IBox<string> under the invariant marker of IBox, so no overload is applicable.
//! cites: 12.9, 14.5
//! expect: reject
//! diagnostic: no overload of `F` is applicable
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
  public static method F(x: IBox<string>): int => 2;
end S;
public class Program
  public static method Main(): int
    using var a := new DogBox();
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
end Program;
