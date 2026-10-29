//! case: l2_5_0044
//! doc: stdlib
//! section: 2.5
//! title: DateTime constructor accepts each boundary value and raises on one past each bound
//! rule: ArgumentOutOfRangeException on invalid components
//! cites: 2.5
//! expect: run
//! output: throw ok ok throw throw
//!   throw ok ok throw
//!   throw ok ok throw
//!   throw ok ok throw
//!   throw ok ok throw
module Conf;

public class Probe
  public static method Mo(m: int): string
    try
      var d := new DateTime(2020, m, 1);
    catch ArgumentOutOfRangeException => return "throw"; end;
    return "ok";
  end Mo;
  public static method Dy(x: int): string
    try
      var d := new DateTime(2020, 1, x);
    catch ArgumentOutOfRangeException => return "throw"; end;
    return "ok";
  end Dy;
  public static method Hr(x: int): string
    try
      var d := new DateTime(2020, 1, 1, x, 0, 0);
    catch ArgumentOutOfRangeException => return "throw"; end;
    return "ok";
  end Hr;
  public static method Mi(x: int): string
    try
      var d := new DateTime(2020, 1, 1, 1, x, 0);
    catch ArgumentOutOfRangeException => return "throw"; end;
    return "ok";
  end Mi;
  public static method Se(x: int): string
    try
      var d := new DateTime(2020, 1, 1, 1, 1, x);
    catch ArgumentOutOfRangeException => return "throw"; end;
    return "ok";
  end Se;
end Probe;

public class Program
  public static method Main()
    Console.WriteLine(Probe.Mo(0) + " " + Probe.Mo(1) + " " + Probe.Mo(12) + " " + Probe.Mo(13) + " " + Probe.Mo(-1));
    Console.WriteLine(Probe.Dy(0) + " " + Probe.Dy(1) + " " + Probe.Dy(31) + " " + Probe.Dy(32));
    Console.WriteLine(Probe.Hr(-1) + " " + Probe.Hr(0) + " " + Probe.Hr(23) + " " + Probe.Hr(24));
    Console.WriteLine(Probe.Mi(-1) + " " + Probe.Mi(0) + " " + Probe.Mi(59) + " " + Probe.Mi(60));
    Console.WriteLine(Probe.Se(-1) + " " + Probe.Se(0) + " " + Probe.Se(59) + " " + Probe.Se(60));
  end Main;
end Program;
