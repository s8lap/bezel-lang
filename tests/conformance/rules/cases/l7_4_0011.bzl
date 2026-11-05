//! case: l7_4_0011
//! doc: stdlib
//! section: 7.4
//! title: Narrowing by an and-condition reaches TextWriter.WriteLine
//! rule: boolean composition of tests: in `A and B`, `B` (and the true branch) is
//!   checked under `A`'s true-facts
//! cites: 7.4
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
    if p <> null and p.X > 0 then
      t.WriteLine(p);
      Console.WriteLine(p);
    end;
  end Show;

  public static method Main()
    Show(new P(1, 2));
  end Main;
end Program;
