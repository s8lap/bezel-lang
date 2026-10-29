//! case: l2_2_0034
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs and Math.Max accept a long? narrowed non-null
//! rule: stdlib §2.2 declares Abs(x: long). §7.4 (v0.350): narrowing is flow-sensitive and "a null test ... `x <> null` ... narrowing the true branch"; a narrowed nullable local has the non-nullable type, so a stdlib parameter of the underlying type accepts it. Abs(-9000000000) is 9000000000 and Max(-9000000000, 5L) is 5.
//! cites: 2.2, 7.4
//! expect: run
//! output: 9000000000
//!   5
module Conf;

public class Program
  public static method IdL(x: long?): long? return x; end IdL;
  public static method Main()
    var l: long? := IdL(-9000000000L);
    if l <> null then
      var r: long := Math.Abs(l);
      Console.WriteLine(r);
      Console.WriteLine(Math.Max(l, 5L));
    end;
  end Main;
end Program;
