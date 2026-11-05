//! case: s26_5_2406
//! doc: spec
//! section: 26.5
//! title: Inside F<T>(Array<Array<T>>) the stdlib Min() is filtered and extension
//!   Object answers
//! rule: A `where` clause is an *applicability* filter, never a ranking signal.
//! cites: 26.5, 14.2
//! expect: run
//! output: 4242
//! issue: 8199
//! notes: Red on base: the saved pre-fix compiler of the #8186/#8199 review refused
//!   this program with the #2770 'Min() without a selector requires the element
//!   type to satisfy T: IComparable<T>' error, before extension resolution
//!   could apply the applicability filter. It passes on the staging tip.
//!   Array<T> is not IComparable<Array<T>> at any T, so the filter drops the
//!   stdlib candidate in the generic body as well.
module Conf;

extension Object
  public method Min(): int => 4242;
end;

public class Program
  public static method F<T>(a: Array<Array<T>>)
    Console.WriteLine(a.Min());
  end F;

  public static method Main(): int
    using var a := new Array<Array<int>>();
    F(a);
    return 0;
  end Main;
end Program;
