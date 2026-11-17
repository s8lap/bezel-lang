//! case: s14_2_4924
//! doc: spec
//! section: 14.2
//! title: a record that declares CompareTo but does not list IComparable<T> does not satisfy Sort()'s bound
//! rule: **T is a record** that lists I (or any interface extending I) in its inheritance clause
//!   and declares matching members
//! cites: 14.2, 4.4
//! expect: reject
//! diagnostic: IComparable
//! issue: 8584
module Conf;

public record P2(a: int, b: int)
  public method CompareTo(other: P2): int
    return b - other.b;
  end CompareTo;
end P2;

public class Program
  public static method Main()
    var a: Array<P2> := [new P2(2, 1), new P2(1, 2), new P2(3, 0)];
    a.Sort();
  end Main;
end Program;
