//! case: l6_5_0014
//! doc: stdlib
//! section: 6.5
//! title: the source classes are invariant in their payload type
//! rule: The source classes are invariant — `Raise(payload: T)` places T in
//!   input position; an exposed source is simply invariant, as its signature
//!   dictates.
//! cites: 6.5
//! expect: reject
//! diagnostic: invariant
module Conf;

public interface IAnimal
  method Speak(): string;
end IAnimal;

public class Dog: IAnimal
  public constructor()
  end constructor;

  public method Speak(): string
    return "woof";
  end Speak;
end Dog;

public class Program
  public static method Demo()
    using var src: EventSource<IAnimal> := new EventSource<Dog>();
    using var sub := src.Subscribe(lambda(a) => Console.WriteLine(a.Speak()));
  end Demo;
end Program;
