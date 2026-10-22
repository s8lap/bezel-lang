//! case: l9_8_0007
//! doc: stdlib
//! section: 9.8
//! title: Range.Contains through a Range<int> field, a method parameter and a call argument
//! rule: Stdlib §9.8 (v0.340): "`Contains(v)` is `true` iff the range is nonempty under iteration and `v` lies within
//!   the endpoint extent (exclusive upper bound excluded) ... `Step` affects membership only through
//!   direction/emptiness." A `Range<int>` held in a class field, passed as a parameter, or its `Contains` result passed on as
//!   a `bool` argument answers the same membership question.
//! cites: 16.3
//! expect: run
//! output: yes
//!   False
//!   True
//!   False
module Conf;

public class H
  public _q: Range<int> := 1..5;
  public method Init(): bool return _q.Contains(3); end Init;
  public static method P(b: bool): string return (if b then "yes" else "no"); end P;
  public static method R(q: Range<int>, x: int): bool return q.Contains(x); end R;
end H;

public class Program
  public static method Main()
    var q := 1..5;
    Console.WriteLine(H.P(q.Contains(2)));
    Console.WriteLine(H.R(q, 7));
    var h := new H();
    Console.WriteLine(h.Init());
    var f := lambda() => h._q.Contains(9);
    Console.WriteLine(f());
  end Main;
end Program;
