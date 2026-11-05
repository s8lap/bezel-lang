//! case: s14_2_2417
//! doc: spec
//! section: 14.2
//! title: A class listing IComparable<K> and declaring CompareTo(K) conforms: Min()
//!   and Max() run
//! rule: that lists I (or any interface extending I) in its inheritance clause
//! cites: 14.2, 26.5
//! expect: run
//! output: 2
//!   8
//! notes: The accepting twin of s14_2_2416: the only edit is the IComparable<K>
//!   clause. Direct conformance, so green on base as well as on the tip.
module Conf;

public class K: IComparable<K>
  private _v: int;

  public constructor(v: int)
    _v := v;
  end constructor;

  public method V(): int => _v;

  public method CompareTo(other: K): int => _v - other._v;
end K;

public class Program
  public static method Main(): int
    using var a := new K(5);
    using var b := new K(2);
    using var c := new K(8);
    var arr: Array<K> := [a, b, c];
    Console.WriteLine(arr.Min().V());
    Console.WriteLine(arr.Max().V());
    return 0;
  end Main;
end Program;
