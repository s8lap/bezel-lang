//! case: s27_1_1_0008
//! doc: spec
//! section: 27.1.1
//! title: a lambda's written narrower parameter type is not bridged at a wider target slot
//! rule: the admission of a lambda literal at a target-typed non-call
//!   position (§27.3) is the one other site-typed bridge — the literal, like
//!   a formed reference, is typed at the site: its body's inferred return
//!   type converts to the target's return slot under §5.1, while a written
//!   parameter type or return ascription is judged as a value's slot under
//!   this subsection ... Parameter slots of class or interface type, nullable
//!   or not, are contravariant: the target's parameter type must reach the
//!   value's by a reference conversion (§5.1), its nullable lift or the
//!   nullable view of a reference type, never the view's reverse
//! cites: 27.1.1, 27.3
//! expect: reject
//! diagnostic: parameter
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
    var f: (Animal) -> string := lambda(d: Dog) => d.Speak();
    using var a := new Animal();
    Console.WriteLine(f(a));
  end Main;
end Program;
