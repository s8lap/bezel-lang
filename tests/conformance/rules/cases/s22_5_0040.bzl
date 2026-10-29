//! case: s22_5_0040
//! doc: spec
//! section: 22.5
//! title: a named-argument call that throws on a later argument has still run the earlier ones
//! rule: Supplied argument expressions are evaluated in call-site textual order; an
//!   earlier argument has run when a later one throws (named arguments included).
//! cites: 22.5
//! expect: run
//! output: AB
module Conf;
import System.Text;

public class Program
  static method TD(sb: StringBuilder, s: string, v: double): double
    sb.Append(s);
    return v;
  end TD;
  static method BoomD(sb: StringBuilder, s: string): double
    sb.Append(s);
    throw new InvalidOperationException("boom" + s);
  end BoomD;

  public static method Main()
    using var sb := new StringBuilder();
    try
      var r := Math.Pow(y := TD(sb, "A", 2.0), x := BoomD(sb, "B"));
      Console.WriteLine(r.ToString());
    catch
      var e: InvalidOperationException => Console.WriteLine(sb.ToString());
    end;
  end Main;
end Program;
