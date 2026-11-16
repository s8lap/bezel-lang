//! case: s14_5_4822
//! doc: spec
//! section: 14.5
//! title: a named string collection literal at an Array<K> formal whose K binds to int is rejected
//! rule: Conversions get their say afterward, argument by argument, at the [§22.6](#sec-overloading) applicability check against the substituted signature
//! cites: 14.5, 22.6
//! expect: reject
//! diagnostic: not implicitly convertible to the target type
//! issue: 8625
module Conf;

record Box(Name: string, Age: int);

public static class Seq
  public static method Flat<T, K>(xs: Array<T>, key: (T) -> K, extra: Array<K>): int
    return extra.Count;
  end Flat;

  public static method Nest<T, K>(xs: Array<T>, key: (T) -> K, extra: Array<Array<K>>): int
    return extra.Count;
  end Nest;

  public static method Opt<T, K>(xs: Array<T>, key: (T) -> K, extra: Array<K>?): int
    return extra?.Count ?? -1;
  end Opt;

  public static method Both<K>(k: K, extra: Array<K>): int
    return extra.Count;
  end Both;
end Seq;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("dan", 35), new Box("bob", 25)];
    Console.WriteLine(Seq.Flat(xs, lambda(v) => v.Age, extra := ["x"]));
  end Main;
end Program;
