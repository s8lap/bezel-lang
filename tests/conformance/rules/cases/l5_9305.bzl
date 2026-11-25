//! case: l5_9305
//! doc: stdlib
//! section: 5
//! title: SelectMany over groups flattens the groups' elements, not their keys
//! rule: public interface IGrouping<TKey, T>: IEnumerable<T>
//! cites: 5
//! expect: run
//! output: sm ann
//!   sm ann2
//!   sn ann
//!   sn ann2
//!   sn bob
//! notes: Harvested from the #8542 review (p4). `SelectMany` over an `IEnumerable<IGrouping<string, Person>>`
//!   flattens each group `g`, an `IEnumerable<Person>`. Groups in first-occurrence order: eng [ann (40),
//!   ann2 (50)], ops [bob (20)]. First loop keeps age > 30: ann, ann2. Second loop projects names over every
//!   group: ann, ann2, bob. `x.age` and `x.name` exist only on Person, never on the string key.
module Conf;
public record Person(name: string, dept: string, age: int);
public class Program
  public static method Main(): int
    using var ps := new Array<Person>();
    ps.Add(new Person("ann", "eng", 40));
    ps.Add(new Person("bob", "ops", 20));
    ps.Add(new Person("ann2", "eng", 50));
    for var e in ps.GroupBy(lambda(q: Person) => q.dept).SelectMany(lambda(g) => g.Where(lambda(x) => x.age > 30)) do
      Console.WriteLine("sm " + e.name);
    end for;
    for var e in ps.GroupBy(lambda(q: Person) => q.dept).SelectMany(lambda(g) => g.Select(lambda(x) => x.name)) do
      Console.WriteLine("sn " + e);
    end for;
    return 0;
  end Main;
end Program;
