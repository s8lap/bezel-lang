//! case: s12_1_0006
//! doc: spec
//! section: 12.1
//! title: an extension member is resolved by the receiver's static type, not dispatched virtually
//! rule: A method, property, or indexer is non-virtual unless marked
//!   virtual -- an extension member has no override relationship at all,
//!   and §26.5 resolves it by the receiver's declared type.
//! cites: 12.1, 26.5
//! expect: run
//! stdout: animal extension
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

extension Animal
  public method Describe(): string => "animal extension";
end;

extension Dog
  public method Describe(): string => "dog extension";
end;

public class Program
  public static method Main()
    using var a: Animal := new Dog();
    Console.WriteLine(a.Describe());
  end Main;
end Program;
