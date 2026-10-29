//! case: s22_5_0037
//! doc: spec
//! section: 22.5
//! title: nested named-argument calls evaluate their supplied arguments in textual order
//! rule: A call's supplied argument expressions are evaluated in call-site textual
//!   order, left to right as written, for positional and named arguments alike;
//!   naming an argument changes which parameter it binds to, never when it runs.
//! cites: 22.5
//! expect: run
//! output: ABCD 16
module Conf;
import System.Text;

public class Program
  static method TD(sb: StringBuilder, s: string, v: double): double
    sb.Append(s);
    return v;
  end TD;

  public static method Main()
    using var sb := new StringBuilder();
    var q := Math.Pow(y := Math.Pow(y := TD(sb, "A", 2.0), x := TD(sb, "B", 2.0)),
                      x := Math.Max(b := TD(sb, "C", 1.0), a := TD(sb, "D", 2.0)));
    Console.WriteLine(sb.ToString() + " " + q.ToString());
  end Main;
end Program;
