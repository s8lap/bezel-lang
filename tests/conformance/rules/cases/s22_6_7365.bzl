//! case: s22_6_7365
//! doc: spec
//! section: 22.6
//! title: a lambda producing an owned Dog selects the `() -> owned Dog`
//!   constructor over `() -> owned Animal`
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 8.4
//! expect: run
//! stdout: B:dog
//! notes: Pins the #6642 fix at a class-typed return slot: `new Dog()` has
//!   type Dog, exact at the Dog slot and a reference conversion at the Animal
//!   slot.
module Conf;

public class Animal
  public constructor() end;
  public virtual method Name(): string return "animal"; end;
end;
public class Dog: Animal
  public constructor() end;
  public override method Name(): string return "dog"; end;
end;

public class K
  public constructor(f: () -> owned Animal) Console.WriteLine("A"); end;
  public constructor(f: () -> owned Dog) using var d := f(); Console.WriteLine("B:" + d.Name()); end;
end;
public static class Program
  public static method Main(): int
    var k := new K(lambda() => new Dog());
    return 0;
  end;
end;
