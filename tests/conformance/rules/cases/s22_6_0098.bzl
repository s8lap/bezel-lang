//! case: s22_6_0098
//! doc: spec
//! section: 22.6
//! title: two covariant interface conversions at one position are equally ranked and ambiguous
//! rule: "two candidates that both require an implicit conversion at the same position are **equally ranked** there, regardless of which conversion each uses" ([§22.6]). Array<Dog> converts to IEnumerable<Animal> and to IEnumerable<Dog>, neither is exact, so the call S.F(v) is ambiguous and a compile error.
//! cites: 12.9, 5.1
//! expect: reject
//! diagnostic: ambiguous call to `F`
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
    using var v := new Array<Dog>();
    Console.WriteLine(S.F(v));
    return 0;
  end Main;
end Program;
