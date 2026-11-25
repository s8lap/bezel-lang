//! case: l5_9303
//! doc: stdlib
//! section: 5
//! title: an IGrouping<string, Person> parameter aggregates over its Person elements
//! rule: public interface IGrouping<TKey, T>: IEnumerable<T>
//! cites: 5
//! expect: run
//! output: eng 40
//!   ops 20
//! notes: Harvested from the #8542 review (p7). `Tot` names the group type explicitly, so `g` is an
//!   `IEnumerable<Person>` and `Sum(lambda(x) => x.age)` reads `Person.age`. Trace: groups in first-occurrence
//!   order, eng then ops; eng holds ann (40), ops holds bob (20); Tot returns 40 and 20.
module Conf;
public record Person(name: string, dept: string, age: int);
public class Program
  static method Tot(g: IGrouping<string, Person>): int
    return g.Sum(lambda(x) => x.age);
  end Tot;
  public static method Main(): int
    using var ps := new Array<Person>();
    ps.Add(new Person("ann", "eng", 40));
    ps.Add(new Person("bob", "ops", 20));
    for var g in ps.GroupBy(lambda(q: Person) => q.dept) do
      Console.WriteLine(g.Key + " " + Tot(g).ToString());
    end for;
    return 0;
  end Main;
end Program;
