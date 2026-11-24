//! case: s27_5_8012
//! doc: spec
//! section: 27.5
//! title: a lambda making two compound assignments to a captured string parameter is read by
//!   a sibling
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!   vice versa.
//! cites: 27.5, 28.9, 22.3
//! expect: run
//! stdout: 3200
//! notes: Pins #9005 (a second closure writing a captured string parameter freed a string
//!   the caller still holds); output only, the corpus runner has no sanitizer leg.
module Conf;
public class Program
  public static method M(p: string): string
    var f1 := lambda(): string => p;
    var f2 := lambda(): string => do p += "z"; p += "y"; return p; end;
    var r := f2();
    return f1() + r;
  end M;
  public static method Run(): int
    var s := $"hi-{1}";
    var t := 0;
    for var j in 0..<200 do
      var o := Program.M(s);
      t := t + o.Length + s.Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
