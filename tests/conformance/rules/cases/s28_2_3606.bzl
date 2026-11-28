//! case: s28_2_3606
//! doc: spec
//! section: 28.2
//! title: a span's Length read before its source's disposal is admitted
//! rule: what is rejected is the use the kill makes certain to fault, and a disposal after the borrow's
//!   last use changes no verdict
//! cites: 28.2, 9.7
//! expect: run
//! stdout: 3
//! issue: 8748
//! notes: Accepting twin of s28_2_3605.
module Conf;

public class Program
  public static method Run(): int
    var xs: Array<int> := [1, 2, 3];
    var s := xs.AsSpan();
    var n := s.Length;
    xs.Dispose();
    return n;
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
