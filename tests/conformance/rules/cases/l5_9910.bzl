//! case: l5_9910
//! doc: stdlib
//! section: 5
//! title: An Aggregate fold over a grouping reads the element's members
//! rule: public method Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum): TAccum;
//! cites: 5
//! expect: run
//! output: a=70
//!   b=25
//! notes: Harvested from the #9198 review (r43-rev-m149-9198). On `IGrouping<TKey, T>: IEnumerable<T>` the
//!   fold's second parameter is the ELEMENT type T, not the key type TKey. Groups appear in
//!   first-occurrence-of-key order (stdlib 5, GroupBy semantics): a holds 30 and 40 (70), b holds 25.
module Conf;
import System.Collections.Linq;

record Person(name: string, age: int);

class C
  public static method Main(): int
    using var ps := new Array<Person>();
    ps.Add(new Person("a", 30));
    ps.Add(new Person("b", 25));
    ps.Add(new Person("a", 40));
    for var g in ps.GroupBy(lambda(p) => p.name) do
      var t := g.Aggregate(0, lambda(acc, x) => acc + x.age);
      Console.WriteLine($"{g.Key}={t}");
    end;
    return 0;
  end Main;
end C;
