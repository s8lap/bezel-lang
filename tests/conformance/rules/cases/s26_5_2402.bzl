//! case: s26_5_2402
//! doc: spec
//! section: 26.5
//! title: Array<Set<int>>: the stdlib Min()/Max() candidate is filtered, extension
//!   Object answers
//! rule: A `where` clause is an *applicability* filter, never a ranking signal.
//! cites: 26.5, 14.2
//! expect: run
//! output: 4242
//!   4343
//! issue: 8199
//! notes: Red on base: the saved pre-fix compiler of the #8186/#8199 review refused
//!   this program with the #2770 'Min() without a selector requires the element
//!   type to satisfy T: IComparable<T>' error, before extension resolution
//!   could apply the applicability filter. It passes on the staging tip. Set<T>
//!   lists no IComparable (stdlib 4.10).
module Conf;

extension Object
  public method Min(): int => 4242;
  public method Max(): int => 4343;
end;

public class Program
  public static method Main(): int
    using var a := new Array<Set<int>>();
    Console.WriteLine(a.Min());
    Console.WriteLine(a.Max());
    return 0;
  end Main;
end Program;
