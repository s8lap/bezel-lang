//! case: s16_3_0007
//! doc: spec
//! section: 16.3
//! title: a Range converts to IEnumerable at a local, an argument, a return, a constructor and a conditional
//! rule: "`Range<T>` is a compiler-intrinsic record that implements `IEnumerable<T>`" (§16.3, v0.340); a value is admitted where an interface it implements is expected (§5.1, v0.340), so a range is accepted wherever an `IEnumerable<T>` is expected and the enumeration yields the range's values: `1..4` sums to 10, `10..1 step -3` yields 10, 7, 4, 1 (22), `3..<3` is empty.
//! cites: 5.1, 9.8
//! expect: run
//! output: 10
//!   9
//!   10
//!   22
//!   0
//!   3
//!   9
//!   10
//!   6
//!   6
module Conf;

public class Box
  private _n: int;
  public constructor(xs: IEnumerable<int>)
    _n := 0;
    for var x in xs do _n += x; end;
  end constructor;
  public property N: int => _n;
end Box;

public class Program
  public static method Sum(xs: IEnumerable<int>): int
    var n := 0;
    for var x in xs do n += x; end;
    return n;
  end Sum;

  public static method Ret(): IEnumerable<int> return 1..4; end Ret;
  public static method RetStep(): IEnumerable<int> return 10..1 step -3; end RetStep;
  public static method RetEmpty(): IEnumerable<int> return 3..<3; end RetEmpty;
  public static method Arrow(): IEnumerable<int> => 0..2;

  public static method Main()
    var a: IEnumerable<int> := 1..4;
    Console.WriteLine(Sum(a));
    Console.WriteLine(Sum(5..1 step -2));
    Console.WriteLine(Sum(Ret()));
    Console.WriteLine(Sum(RetStep()));
    Console.WriteLine(Sum(RetEmpty()));
    Console.WriteLine(Sum(Arrow()));
    var rv: Range<int> := 1..6 step 2;
    var d: IEnumerable<int> := rv;
    Console.WriteLine(Sum(d));
    Console.WriteLine(new Box(1..4).N);
    var cond: IEnumerable<int> := (if Sum(a) > 5 then 1..3 else 4..5);
    Console.WriteLine(Sum(cond));
    var longs: IEnumerable<long> := 1L..3L;
    var t: long := 0;
    for var x in longs do t += x; end;
    Console.WriteLine(t);
  end Main;
end Program;
