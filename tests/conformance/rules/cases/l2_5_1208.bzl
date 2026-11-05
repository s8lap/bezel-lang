//! case: l2_5_1208
//! doc: stdlib
//! section: 2.5
//! title:Sorting 1000 pseudo-random TimeSpans and DateTimes 200 times leaves every adjacent pair
//!   ordered
//! rule:CompareTo = tick order. Ordering is CompareTo only
//! cites: 4.4
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var bad := 0;
    for var round in 0 ..< 200 do
      using var a := new Array<TimeSpan>();
      using var d := new Array<DateTime>();
      var seed := 12345L + round;
      for var i in 0 ..< 1000 do
        seed := (seed * 1103515245L + 12345L) mod 2147483648L;
        var t := (seed - 1073741824L) * 4294967311L;
        a.Add(TimeSpan.FromTicks(t));
        var u := t;
        if u < 0L then
          u := -u;
        end if;
        d.Add(DateTime.FromTicks(u mod 3155378975999999999L));
      end for;
      a.Sort();
      var ds := d.Sorted();
      for var i in 1 ..< 1000 do
        if a[i - 1].Ticks > a[i].Ticks then
          bad := bad + 1;
        end if;
        if ds[i - 1].Ticks > ds[i].Ticks then
          bad := bad + 1;
        end if;
      end for;
    end for;
    Console.WriteLine(bad);
  end Main;
end Program;
