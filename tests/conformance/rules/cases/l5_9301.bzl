//! case: l5_9301
//! doc: stdlib
//! section: 5
//! title: a lambda over a group sees the element type, so a member only the key has is rejected
//! rule: public interface IGrouping<TKey, T>: IEnumerable<T>
//! cites: 5
//! expect: reject
//! diagnostic: length
//! notes: Harvested from the #8542 review (n1). The group `g` is an `IGrouping<string, Person>`, which is an
//!   `IEnumerable<Person>`, so the lambda parameter `x` of `g.Count(...)` is a `Person`, not the string key.
//!   `Person` has no `Length`; the only ground for rejection is `x.Length`. The accepting twin is l5_9302.
module Conf;
public record Person(name: string, dept: string, age: int);
public class Program
  public static method Main(): int
    using var ps := new Array<Person>();
    ps.Add(new Person("ann", "eng", 40));
    for var g in ps.GroupBy(lambda(q: Person) => q.dept) do
      Console.WriteLine(g.Count(lambda(x) => x.Length > 1));
    end for;
    return 0;
  end Main;
end Program;
