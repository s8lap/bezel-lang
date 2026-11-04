//! case: s2_3_0801
//! doc: spec
//! section: 2.3
//! title: a local Math of a user class receives every overload call instead of the built-in Math
//! rule: every other always-available name may be shadowed by local declarations under the normal name-resolution rules
//! cites: 2.3, 2.7
//! expect: run
//! stdout: UMAX|UMIN|UABS|USIGN|UCLAMP|UCEIL|UABSB|UMAXD
//! issue: 8019
//! notes: every result is the user method's string; a built-in Math.Max would return a number, Math.Abs(byte) selects the byte overload.
module Conf;

public class Sh
  public method Max(a: int, b: int): string
    return "UMAX";
  end Max;
  public method Min(a: int, b: int): string
    return "UMIN";
  end Min;
  public method Abs(a: int): string
    return "UABS";
  end Abs;
  public method Abs(a: byte): string
    return "UABSB";
  end Abs;
  public method Sign(a: int): string
    return "USIGN";
  end Sign;
  public method Clamp(a: int, b: int, c: int): string
    return "UCLAMP";
  end Clamp;
  public method Ceiling(a: decimal): string
    return "UCEIL";
  end Ceiling;
  public method Max(a: decimal, b: decimal): string
    return "UMAXD";
  end Max;
end Sh;

public class Program
  public static method Main()
    var Math := new Sh();
    var a: byte := 200;
    var d1: decimal := 1.5m;
    var d2: decimal := 2.5m;
    Console.WriteLine(Math.Max(1, 2) + "|" + Math.Min(1, 2) + "|" + Math.Abs(-3) + "|" + Math.Sign(-3) + "|" + Math.Clamp(1, 2, 3) + "|" + Math.Ceiling(d1) + "|" + Math.Abs(a) + "|" + Math.Max(d1, d2));
  end Main;
end Program;
