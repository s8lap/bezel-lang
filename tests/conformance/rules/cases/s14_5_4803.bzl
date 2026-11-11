//! case: s14_5_4803
//! doc: spec
//! section: 14.5
//! title: an empty collection literal at an Array<K> formal is typed once K binds from the lambda return
//! rule: Conversions get their say afterward, argument by argument, at the [§22.6](#sec-overloading) applicability check against the substituted signature
//! cites: 14.5, 15.4
//! expect: run
//! stdout: dan
module Conf;

record Box(Name: string, Age: int);

public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K, extra: Array<K>): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then
        best := x;
        bk := k;
      end;
    end;
    return best;
  end M;
end Seq;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("dan", 35), new Box("bob", 25), new Box("cho", 40)];
    Console.WriteLine(Seq.M(xs, lambda(v) => v.Name + "!", []).Name);
  end Main;
end Program;
