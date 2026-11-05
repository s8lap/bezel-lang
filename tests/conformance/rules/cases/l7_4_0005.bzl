//! case: l7_4_0005
//! doc: stdlib
//! section: 7.4
//! title: A value-like choice narrowed by a null test is written through TextWriter.WriteLine
//! rule: public method WriteLine(value: Object);
//! cites: 7.4, 5.1, 35.7
//! expect: run
//! stdout: VC.A(x: 3)
//! issue: 7965
//! notes: twin of l7_4_0008 (pairs the accepted with its rejecting form).
module Conf;
import System.IO;

public choice VC A(x: int), B; end VC;

public class Program
  public static method Show(p: VC?)
    var t: TextWriter := Console.Out;
    if p <> null then
      t.WriteLine(p);
    end;
  end Show;

  public static method Main()
    Show(VC.A(3));
  end Main;
end Program;
