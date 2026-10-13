//! case: s27_1_1_0007
//! doc: spec
//! section: 27.1.1
//! title: a lambda's written wider parameter type is admitted at a narrower target slot
//! rule: the admission of a lambda literal at a target-typed non-call position
//!   (§27.3) is the one other site-typed bridge — the literal, like a formed
//!   reference, is typed at the site: its body's inferred return type converts
//!   to the target's return slot under §5.1, while a written parameter type or
//!   return ascription is judged as a value's slot under this subsection ... a `(Animal) -> ()` value is usable where `(Dog) -> ()` is expected
//! cites: 27.1.1, 27.3
//! expect: run
//! stdout: dog
module Conf;

public class Animal
  public constructor()
  end constructor;

  public virtual method Speak(): string => "animal";
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  public override method Speak(): string => "dog";
end Dog;

public class Program
  public static method Main()
    var f: (Dog) -> string := lambda(a: Animal) => a.Speak();
    using var d := new Dog();
    Console.WriteLine(f(d));
  end Main;
end Program;
