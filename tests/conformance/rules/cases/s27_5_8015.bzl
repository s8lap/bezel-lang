//! case: s27_5_8015
//! doc: spec
//! section: 27.5
//! title: a returned lambda growing its own captured string parameter reads the parameter's
//!   length each call
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!   vice versa.
//! cites: 27.5, 28.9, 22.3
//! expect: run
//! stdout: 1000
//! notes: Pins #9005 (a second closure writing a captured string parameter freed a string
//!   the caller still holds); output only, the corpus runner has no sanitizer leg.
module Conf;
public class Program
  public static method Ident(p: string): () -> string
    return lambda(): string => do p := p + "z"; return p; end;
  end Ident;
  public static method Run(): int
    var s := $"hi-{1}";
    var t := 0;
    for var j in 0..<200 do
      var f := Program.Ident(s);
      t := t + f().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
