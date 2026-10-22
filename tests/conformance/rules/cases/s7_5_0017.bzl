//! case: s7_5_0017
//! doc: spec
//! section: 7.5
//! title: a null-conditional access to an undeclared grouping member is refused
//! rule: "`x?.M` returns null if `x` is null; otherwise it performs the access" ([§7.5]). The grouping is not null here, so `g?.Foo` performs the access, and "readonly property Key: TKey;" ([stdlib §5]) is the only property "public interface IGrouping<TKey, T>: IEnumerable<T>" declares, so `Foo` names no member and the access is refused.
//! cites: 5
//! expect: reject
//! diagnostic: it declares only `Key`
module Conf;
public class C
  public static method Main(): int
    var a: Array<int> := [1, 2, 2, 3];
    for var g in a.GroupBy(lambda(x: int) => x) do
      Console.WriteLine(g?.Foo);
    end;
    return 0;
  end Main;
end C;
