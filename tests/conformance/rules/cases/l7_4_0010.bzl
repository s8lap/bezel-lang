//! case: l7_4_0010
//! doc: stdlib
//! section: 7.4
//! title: Narrowing by early return reaches TextWriter.WriteLine and Console.WriteLine alike
//! rule: an early exit: after `if x = null then return; end;`
//! cites: 7.4, 2.1
//! expect: run
//! output: P(X: 1, Y: 2)
//!   P(X: 1, Y: 2)
//! issue: 7965
//! notes: twin of l7_4_0008 (pairs the accepted with its rejecting form).
module Conf;
import System.IO;

public record P(X: int, Y: int);

public class Program
  public static method Show(p: P?)
    var t: TextWriter := Console.Out;
    if p = null then
      return;
    end;
    t.WriteLine(p);
    Console.WriteLine(p);
  end Show;

  public static method Main()
    Show(new P(1, 2));
  end Main;
end Program;
