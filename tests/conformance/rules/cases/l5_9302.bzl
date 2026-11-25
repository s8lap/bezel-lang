//! case: l5_9302
//! doc: stdlib
//! section: 5
//! title: a lambda over a group reads the element's own members, not the key's
//! rule: public interface IGrouping<TKey, T>: IEnumerable<T>
//! cites: 5
//! expect: run
//! output: eng 1
//!   ops 0
//! notes: Accepting twin of l5_9301 (differs only in `x.age > 30` for `x.Length > 1`). Trace: GroupBy by
//!   department gives groups in first-occurrence order, eng then ops (stdlib 5 GroupBy semantics). The `eng`
//!   group holds Person(ann, 40) and Person(bob2, 20): exactly one has age > 30, so Count is 1. The `ops`
//!   group holds Person(bob, 20): none, so 0. The key type (string) and element type (Person) differ, so
//!   reading `x.age` is only possible if `x` is the element.
module Conf;
public record Person(name: string, dept: string, age: int);
public class Program
  public static method Main(): int
    using var ps := new Array<Person>();
    ps.Add(new Person("ann", "eng", 40));
    ps.Add(new Person("bob", "ops", 20));
    ps.Add(new Person("bob2", "eng", 20));
    for var g in ps.GroupBy(lambda(q: Person) => q.dept) do
      Console.WriteLine(g.Key + " " + g.Count(lambda(x) => x.age > 30).ToString());
    end for;
    return 0;
  end Main;
end Program;
