//! case: l5_0022
//! doc: stdlib
//! section: 5
//! title: a grouping-typed parameter does not expose a member only the implementing class declares
//! rule: "A class implements an interface implicitly: by declaring matching public members" ([§11.2]). The parameter is typed "public interface IGrouping<TKey, T>: IEnumerable<T>", whose declared surface is "readonly property Key: TKey;" ([stdlib §5]); the `Extra` property that only `MyG` declares is not on that surface, so `g.Extra` names no member and is refused.
//! cites: 11.2
//! expect: reject
//! diagnostic: it declares only `Key`
module Conf;
public class MyG: IGrouping<int, int>
  public property Key: int => 1;
  public property Extra: int => 9;
  public method GetEnumerator(): owned IEnumerator<int> => new Array<int>().GetEnumerator();
end MyG;
public class C
  public static method Show(g: IGrouping<int, int>): int
    return g.Extra;
  end Show;
  public static method Main(): int
    Console.WriteLine(Show(new MyG()));
    return 0;
  end Main;
end C;
