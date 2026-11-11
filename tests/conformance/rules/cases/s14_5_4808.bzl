//! case: s14_5_4808
//! doc: spec
//! section: 14.5
//! title: an empty literal at an Array<U> formal whose U nothing else binds is an inference failure
//! rule: A type parameter that remains unbound
//! cites: 14.5, 15.4
//! expect: reject
//! diagnostic: cannot infer the type argument
module Conf;

record Box(Name: string, Age: int);

public static class Seq
  public static method M<T, K, U>(xs: Array<T>, key: (T) -> K, extra: Array<U>): T where K: IComparable<K>
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
    Console.WriteLine(Seq.M(xs, lambda(v) => v.Age, []).Name);
  end Main;
end Program;
