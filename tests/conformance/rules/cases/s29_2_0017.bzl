//! case: s29_2_0017
//! doc: spec
//! section: 29.2
//! title: an iterator's range-for fixes its bound once, past a body write and a side-effecting bound call
//! rule: When source is a Range<T> value (§16.3), the compiler may replace the
//!   entire desugaring above with a direct counted loop, provided the
//!   observable behavior is identical.
//! cites: 29.2, 16.5, 16.3
//! expect: run
//! output: 0
//!   1
//!   2
//!   10
//!   11
//!   1
//! issue: 6950
module Conf;

public class Program
  public static calls: int := 0;

  public static method Bound(n: int): int
    calls := calls + 1;
    return n;
  end Bound;

  public static method Gen(): IEnumerable<int>
    var h := 2;
    for var i in 0..h do
      h := 0;
      yield return i;
    end;
    for var j in 0..<Bound(2) do
      yield return 10 + j;
    end;
  end Gen;

  public static method Main()
    for var v in Gen() do
      Console.WriteLine(v);
    end;
    Console.WriteLine(calls);
  end Main;
end Program;
