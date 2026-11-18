//! case: s7_5_7640
//! doc: spec
//! section: 7.5
//! title: `?.` on a record-returning method yields a nullable record and `??` supplies the fallback
//! rule: The result type is always nullable
//! cites: 7.5
//! expect: run
//! output: 7
//!   30
module Conf;
import System;
record Pt(X: int, Y: int);
public class Helper
  public method Mk(): Pt
    return new Pt(3, 4);
  end Mk;
end Helper;
public class Program
  public static method Main()
    var h: Helper? := new Helper();
    var r: Pt := h?.Mk() ?? new Pt(0, 0);
    Console.WriteLine(r.X + r.Y);
    var h2: Helper? := null;
    var r2: Pt := h2?.Mk() ?? new Pt(10, 20);
    Console.WriteLine(r2.X + r2.Y);
  end Main;
end Program;
