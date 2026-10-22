//! case: l5_0021
//! doc: stdlib
//! section: 5
//! title: a grouping declares only Key, so an access to any other member name is refused
//! rule: "public interface IGrouping<TKey, T>: IEnumerable<T>" declares "readonly property Key: TKey;" and no other property, so the access `g.Foo` names no member of the grouping ([§5]).
//! expect: reject
//! diagnostic: it declares only `Key`
module Conf;
public class C
  public static method Main(): int
    var a: Array<int> := [1, 2, 2, 3];
    for var g in a.GroupBy(lambda(x: int) => x) do
      Console.WriteLine(g.Foo);
    end;
    return 0;
  end Main;
end C;
