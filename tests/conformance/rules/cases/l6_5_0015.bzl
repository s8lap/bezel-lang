//! case: l6_5_0015
//! doc: stdlib
//! section: 6.5
//! title: a covariant IEvent face over a narrower source is directly declarable
//! rule: IEvent<T> and IAsyncEvent<T> are covariant in T (`out T`). Covariant
//!   exposure is directly declarable by the pattern: an EventSource<Dog> field
//!   behind a `property Sightings: IEvent<Animal>` face.
//! cites: 6.5
//! expect: run
//! stdout: woof
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

public class Sanctuary
  private owned readonly _seen: EventSource<Dog> := new();
  public property Sightings: IEvent<IAnimal> => _seen;

  public constructor()
  end constructor;

  public method Report(d: Dog)
    _seen.Raise(d);
  end Report;
end Sanctuary;

public class Program
  public static method Main()
    using var s := new Sanctuary();
    using var sub := s.Sightings.Subscribe(lambda(a) => Console.WriteLine(a.Speak()));
    using var d := new Dog();
    s.Report(d);
  end Main;
end Program;
