//! case: l5_0009
//! doc: stdlib
//! section: 5
//! title: OrderBy composed with ThenBy is stable at every tier
//! rule: The OrderBy family is STABLE; ThenBy/ThenByDescending break ties
//!   left by the preceding key while preserving encounter order among
//!   equal elements.
//! cites: 5
//! expect: run
//! stdout: Bo
module Conf;

public record Person(Name: string, Age: int);

public class Program
  public static method Main()
    using var people: Array<Person> := [
      new Person("Ann", 30),
      new Person("Bo", 25),
      new Person("Cy", 25)
    ];
    using var sorted := people.OrderBy(lambda(p: Person): int => 0)
                               .ThenBy(lambda(p: Person): int => p.Age)
                               .ToArray();
    Console.WriteLine(sorted[0].Name);
  end Main;
end Program;
