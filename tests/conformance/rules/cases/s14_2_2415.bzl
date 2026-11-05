//! case: s14_2_2415
//! doc: spec
//! section: 14.2
//! title: A class with no IComparable<K> in its inheritance clause and no CompareTo
//!   has no Min()
//! rule: that lists I (or any interface extending I) in its inheritance clause
//! cites: 14.2, 26.5
//! expect: reject
//! diagnostic: IComparable
//! notes: Single ground: K lists no IComparable<K>. Deleting the Min() line leaves a
//!   compiling file. The twin with the clause is the next case. Not red on
//!   base: the pre-fix compiler also rejected it.
module Conf;

public class K
  private _v: int;

  public constructor(v: int)
    _v := v;
  end constructor;

  public method V(): int => _v;
end K;

public class Program
  public static method Main(): int
    using var k := new K(1);
    var arr: Array<K> := [k];
    Console.WriteLine(arr.Min().V());
    return 0;
  end Main;
end Program;
