//! case: l2_2_2007
//! doc: stdlib
//! section: 2.2
//! title: Math.Max over Guid values read from an array element accumulates
//!   the greatest across 300 iterations
//! rule: `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b`
//!   otherwise; `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and
//!   `b` otherwise
//! cites: 2.6, 4.4
//! expect: run
//! output: 22222222-2222-2222-2222-222222222222
//! notes: stdlib 2.6: Guid CompareTo is lexicographic over the 16 canonical
//!   big-endian bytes (the textual hex order), unsigned. Failed on the
//!   compiler before the #7935 fix and passes on the staging tip.
module Conf;

public class Program
  public static method Main()
    var arr: Array<Guid> := [Guid.Parse("11111111-1111-1111-1111-111111111111"), Guid.Parse("22222222-2222-2222-2222-222222222222")];
    var acc := Guid.Empty;
    for var i in 0..<300 do
      acc := Math.Max(acc, arr[i mod 2]);
    end;
    Console.WriteLine(acc.ToString());
  end Main;
end Program;
