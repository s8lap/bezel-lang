//! case: l5_9912
//! doc: stdlib
//! section: 5
//! title: An Aggregate fold over an int-keyed grouping of records folds over the element type
//! rule: public method Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum): TAccum;
//! cites: 5
//! expect: run
//! output: 30a
//!   25b
//!   40a
//! notes: Harvested from the #9198 review (r43-rev-m149-9198). On `IGrouping<TKey, T>: IEnumerable<T>` the
//!   fold's second parameter is the ELEMENT type T, not the key type TKey. Three int-keyed groups in
//!   first-occurrence order (30, 25, 40), each holding one Person: the string fold appends the
//!   element's name after the key.
module Conf;
import System.Collections.Linq;

record Person(name: string, age: int);

class C
  public static method Main(): int
    using var ps := new Array<Person>();
    ps.Add(new Person("a", 30));
    ps.Add(new Person("b", 25));
    ps.Add(new Person("a", 40));
    for var g in ps.GroupBy(lambda(p) => p.age) do
      var t := g.Aggregate("", lambda(acc, x) => acc + x.name);
      Console.WriteLine(g.Key.ToString() + t);
    end;
    return 0;
  end Main;
end C;
