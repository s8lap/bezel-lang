//! case: l7_4_0012
//! doc: stdlib
//! section: 7.4
//! title: A narrowed guard skips the write when the value is null
//! rule: a null test in either spelling
//! cites: 7.4
//! expect: run
//! output: P(X: 1, Y: 2)
//!   end
//!   end
//! issue: 7965
//! notes: twin of l7_4_0008 (pairs the accepted with its rejecting form).
module Conf;
import System.IO;

public record P(X: int, Y: int);

public class Program
  public static method Show(p: P?)
    var t: TextWriter := Console.Out;
    if p <> null then
      t.WriteLine(p);
    end;
    Console.WriteLine("end");
  end Show;

  public static method Main()
    Show(new P(1, 2));
    Show(null);
  end Main;
end Program;
