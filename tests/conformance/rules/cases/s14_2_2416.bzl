//! case: s14_2_2416
//! doc: spec
//! section: 14.2
//! title: A class declaring CompareTo but not listing IComparable<K> does not
//!   conform: Min() is unavailable
//! rule: that lists I (or any interface extending I) in its inheritance clause
//! cites: 14.2, 26.5
//! expect: reject
//! diagnostic: IComparable
//! notes: Single ground: conformance is nominal, so a matching CompareTo(K) without
//!   IComparable<K> in the inheritance clause gives no conformance (rule 1
//!   requires the clause). Deleting the Min() line leaves a compiling file. Not
//!   red on base: the pre-fix compiler also rejected it.
module Conf;

public class K
  private _v: int;

  public constructor(v: int)
    _v := v;
  end constructor;

  public method V(): int => _v;

  public method CompareTo(other: K): int => _v - other._v;
end K;

public class Program
  public static method Main(): int
    using var k := new K(1);
    var arr: Array<K> := [k];
    Console.WriteLine(arr.Min().V());
    return 0;
  end Main;
end Program;
