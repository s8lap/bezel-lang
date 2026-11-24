//! case: s27_5_8013
//! doc: spec
//! section: 27.5
//! title: a returned closure over a lambda that grows a captured string parameter yields
//!   growing lengths
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!   vice versa.
//! cites: 27.5, 28.9, 22.3
//! expect: run
//! stdout: 8000
//! notes: Pins #9005 (a second closure writing a captured string parameter freed a string
//!   the caller still holds); output only, the corpus runner has no sanitizer leg.
module Conf;
public class Program
  public static method M(p: string): () -> string
    var f1 := lambda(): string => p;
    var f2 := lambda(): string => do p := p + "z"; return p; end;
    return lambda(): string => f2() + f1();
  end M;
  public static method Run(): int
    var s := $"hi-{1}";
    var t := 0;
    for var j in 0..<200 do
      var g := Program.M(s);
      t := t + g().Length + g().Length + g().Length + s.Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
