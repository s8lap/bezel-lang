//! case: l7_4_0009
//! doc: stdlib
//! section: 7.4
//! title: A narrowed record is written through TextWriter.Write as an Object
//! rule: public method Write(value: Object);
//! cites: 7.4, 5.1
//! expect: run
//! stdout: P(X: 1, Y: 2)
//! issue: 7965
//! notes: twin of l7_4_0008 (pairs the accepted with its rejecting form).
module Conf;
import System.IO;

public record P(X: int, Y: int);

public class Program
  public static method Show(p: P?)
    var t: TextWriter := Console.Out;
    if p <> null then
      t.Write(p);
      t.WriteLine();
    end;
  end Show;

  public static method Main()
    Show(new P(1, 2));
  end Main;
end Program;
