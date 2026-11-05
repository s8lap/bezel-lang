//! case: l7_4_0007
//! doc: stdlib
//! section: 7.4
//! title: A narrowed decimal? is written through TextWriter.WriteLine
//! rule: public method WriteLine(value: Object);
//! cites: 7.4, 5.1
//! expect: run
//! stdout: 1.5
//! issue: 7965
//! notes: twin of l7_4_0008 (pairs the accepted with its rejecting form).
module Conf;
import System.IO;

public class Program
  public static method Show(p: decimal?)
    var t: TextWriter := Console.Out;
    if p <> null then
      t.WriteLine(p);
    end;
  end Show;

  public static method Main()
    Show(1.5m);
  end Main;
end Program;
