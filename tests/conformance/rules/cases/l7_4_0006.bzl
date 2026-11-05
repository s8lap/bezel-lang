//! case: l7_4_0006
//! doc: stdlib
//! section: 7.4
//! title: A narrowed generic record reaches TextWriter.WriteLine as an Object
//! rule: public method WriteLine(value: Object);
//! cites: 7.4, 5.1
//! expect: run
//! stdout: G-box
//! issue: 7965
//! notes: twin of l7_4_0008 (pairs the accepted with its rejecting form).
module Conf;
import System.IO;

public record G<T>(V: T)
  public override method ToString(): string => "G-box";
end G;

public class Program
  public static method ShowG(p: G<int>?)
    var t: TextWriter := Console.Out;
    if p <> null then
      t.WriteLine(p);
    end;
  end ShowG;

  public static method Main()
    ShowG(new G<int>(4));
  end Main;
end Program;
