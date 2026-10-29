//! case: s22_5_0041
//! doc: spec
//! section: 22.5
//! title: a null-conditional call evaluates its named arguments only when the receiver is non-null
//! rule: Supplied argument expressions are evaluated in call-site textual order; a
//!   null-conditional call short-circuits (the argument expressions of the skipped suffix are not
//!   evaluated, spec 7.5), so its arguments do not run on a null receiver.
//! cites: 7.5, 22.5
//! expect: run
//! output: AB aaa
//!   AB
module Conf;
import System.Text;

public class Program
  static method Sep(sb: StringBuilder, s: string): string
    sb.Append(s);
    return "-" + s;
  end Sep;

  public static method Main()
    using var one := new StringBuilder();
    var z: StringBuilder? := new StringBuilder("aaa");
    z?.Replace(newValue := Sep(one, "A"), oldValue := Sep(one, "B"));
    Console.WriteLine(one.ToString() + " " + z!.ToString());
    var n2: StringBuilder? := null;
    n2?.Replace(newValue := Sep(one, "C"), oldValue := Sep(one, "D"));
    Console.WriteLine(one.ToString());
  end Main;
end Program;
