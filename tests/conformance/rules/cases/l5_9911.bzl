//! case: l5_9911
//! doc: stdlib
//! section: 5
//! title: An Aggregate fold over a grouping does not see the key as the element
//! rule: public method Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum): TAccum;
//! cites: 5, 26.5
//! expect: reject
//! diagnostic: Length
//! notes: Harvested from the #9198 review (r43-rev-m149-9198). On `IGrouping<TKey, T>: IEnumerable<T>` the
//!   fold's second parameter is the ELEMENT type T, not the key type TKey. The element is a Person,
//!   which has no member Length (26.5 rule 2: a name no rule supplies is a compile error); the string
//!   key's Length must not be read through x. Accepting twin: l5_9910.
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
      var t := g.Aggregate(0, lambda(acc, x) => acc + x.Length);
      Console.WriteLine(t.ToString());
    end;
    return 0;
  end Main;
end C;
