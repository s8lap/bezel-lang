//! case: s7_4_0119
//! doc: spec
//! section: 7.4
//! title: a type fact composes through an early exit
//! rule: spec §7.4: "an early exit: after `if x = null then return; end;` ...
//!     the surviving path is narrowed — and a type fact (§12.7) composes the
//!     same way: after `if x is not T then return; end;`, with the same exits
//!     and through `elif`/`else` branches that all exit, ... the surviving path
//!     is narrowed to `T`". After the `is not Dog` test the surviving path
//!     carries the Dog fact, so the `Animal?` parameter is a `Dog` there and is
//!     exact at the `Dog` parameter.
//! cites: 7.4, 12.7, 22.6
//! expect: run
//! stdout: dog
//! issue: 7302
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public class Program
  public static method Take(x: Dog): string
    return "dog";
  end Take;

  public static method Run(a: Animal?): string
    if a is not Dog then
      return "not a dog";
    end;
    return Take(a);
  end Run;

  public static method Main()
    using var d: Dog := new Dog();
    Console.WriteLine(Run(d));
  end Main;
end Program;
