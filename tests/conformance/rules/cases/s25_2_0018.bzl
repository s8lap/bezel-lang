//! case: s25_2_0018
//! doc: spec
//! section: 25.2
//! title: a derived-typed right operand satisfies an interface-typed operator parameter
//! rule: "classes by declaration or inheritance" ([§5.1]): `Dog` inherits from `Animal`, which
//!   implements `IA`, so a `Dog` operand converts to the `IA` parameter.
//! cites: 25.2, 5.1
//! expect: run
//! stdout: 1001
//! issue: 6217
module Conf;

public interface IA
end IA;

public class Animal: IA
end Animal;

public class Dog: Animal
end Dog;

public record V(X: int)
  public static method operator +(v: V, p: IA): V => new V(v.X + 1000);
end V;

public class Program
  public static method Main()
    var v := new V(1);
    using var d := new Dog();
    Console.WriteLine((v + d).X);
  end Main;
end Program;
