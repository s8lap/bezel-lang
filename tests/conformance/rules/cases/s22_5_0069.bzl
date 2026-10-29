//! case: s22_5_0069
//! doc: spec
//! section: 22.5
//! title: all constructor arguments are evaluated in written order before an out-of-range component raises
//! rule: A call's **supplied** argument expressions are evaluated in **call-site textual order**
//! cites: 22.5, 2.5
//! expect: run
//! output: y
//!   m
//!   d
//!   h
//!   mi
//!   s
//!   caught
//!   d
//!   m
//!   y
//!   caught2
module Conf;

public class Util
  public static method F(n: string, v: int): int
    Console.WriteLine(n);
    return v;
  end F;
end Util;

public class Program
  public static method Main()
    try
      var d := new DateTime(Util.F("y", 2020), Util.F("m", 13), Util.F("d", 1), Util.F("h", 1), Util.F("mi", 1), Util.F("s", 1));
    catch ArgumentOutOfRangeException => Console.WriteLine("caught"); end;
    try
      var d := new DateOnly(day := Util.F("d", 40), month := Util.F("m", 1), year := Util.F("y", 2020));
    catch ArgumentOutOfRangeException => Console.WriteLine("caught2"); end;
  end Main;
end Program;
