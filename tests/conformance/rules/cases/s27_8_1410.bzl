//! case: s27_8_1410
//! doc: spec
//! section: 27.8
//! title: control: generic aliases at generic method parameters, with reordered and renamed parameters
//! rule: Each instantiation of the alias is interchangeable with the corresponding instantiation of
//!   the aliased type.
//! cites: 27.8
//! expect: run
//! output: 1
//!   1
//!   1
//!   hi
//!   5
module Conf;
type Counts<T> := Dictionary<T, int>;
type Pairs<K, V> := Dictionary<K, V>;
public class U
  public static method Count<T>(d: Dictionary<T, int>): int
    return d.Count;
  end Count;
  public static method CountA<T>(d: Counts<T>): int
    return d.Count;
  end CountA;
  public static method CountB<K, V>(d: Pairs<V, K>): int
    return d.Count;
  end CountB;
  public static method First<T>(a: Array<T>): T
    return a[0];
  end First;
  public static method Make<T>(x: T): owned Array<T>
    var a := new Array<T>();
    a.Add(x);
    return a;
  end Make;
end U;
public class Program
  public static method Main()
    var d := new Dictionary<string, int>();
    d["k"] := 1;
    Console.WriteLine(U.Count<string>(d));
    Console.WriteLine(U.CountA<string>(d));
    Console.WriteLine(U.CountB<int, string>(d));
    var a := new Array<string>();
    a.Add("hi");
    Console.WriteLine(U.First<string>(a));
    using var m := U.Make<int>(5);
    Console.WriteLine(m[0]);
  end Main;
end Program;
