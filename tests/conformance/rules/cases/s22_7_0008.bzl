//! case: s22_7_0008
//! doc: spec
//! section: 22.7
//! title: a dispatching call on this from a sealed non-abstract constructor body is admitted
//! rule: "A call from a constructor body to a `virtual`, `override`, or
//!   `abstract` member with receiver `this` — any dispatching call not
//!   through `base.` — is a compile error unless the declaring class is
//!   `sealed` and not `abstract`. In a sealed, non-abstract class the
//!   dispatch target is statically bounded and the call is admitted under
//!   rule 1's gate."
//! cites: 22.7, 12.4
//! expect: run
//! output: woof
module Conf;

public class Animal
  public constructor()
  end constructor;

  public virtual method Speak(): string => "generic";
end Animal;

public sealed class Dog: Animal
  public constructor()
    Console.WriteLine(this.Speak());
  end constructor;

  public override method Speak(): string => "woof";
end Dog;

public class Program
  public static method Main()
    using var d := new Dog();
  end Main;
end Program;
