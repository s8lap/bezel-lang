//! case: s14_5_4823
//! doc: spec
//! section: 14.5
//! title: int literals at Array<K>, Array<Array<K>> and Array<K>? formals agree with a K bound to int
//! rule: Conversions get their say afterward, argument by argument, at the [§22.6](#sec-overloading) applicability check against the substituted signature
//! cites: 14.5, 22.6
//! expect: run
//! output: 2
//!   2
//!   1
//!   -1
//!   3
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
    Console.WriteLine(Seq.Flat(xs, lambda(v) => v.Age, [1, 2]));
    Console.WriteLine(Seq.Nest(xs, lambda(v) => v.Age, [[1], [2, 3]]));
    Console.WriteLine(Seq.Opt(xs, lambda(v) => v.Age, [1]));
    Console.WriteLine(Seq.Opt(xs, lambda(v) => v.Age, null));
    Console.WriteLine(Seq.Both(5, [1, 2, 3]));
  end Main;
end Program;
