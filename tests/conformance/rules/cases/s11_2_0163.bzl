//! case: s11_2_0163
//! doc: spec
//! section: 11.2
//! title: an implementing class answers its own property and passes as the interface for its declared key
//! rule: "Interface members are implicitly public, have no body, and end with `;`" and "A class implements an interface implicitly: by declaring matching public members" ([§11.2]). `MyG` declares "readonly property Key: TKey;" ([stdlib §5]) as the interface requires and also a property `Extra` of its own. Through the class-typed local `Extra` reads 9; passed as the grouping parameter, the interface's `Key` has no body of its own, so it reads the class's `Key`, 1.
//! cites: 5
//! expect: run
//! issue: 6785
//! output: 9
//!   1
module Conf;
public class MyG: IGrouping<int, int>
  public property Key: int => 1;
  public property Extra: int => 9;
  public method GetEnumerator(): owned IEnumerator<int> => new Array<int>().GetEnumerator();
end MyG;
public class C
  public static method Show(g: IGrouping<int, int>): int
    return g.Key;
  end Show;
  public static method Main(): int
    var m := new MyG();
    Console.WriteLine(m.Extra);
    Console.WriteLine(Show(m));
    return 0;
  end Main;
end C;
