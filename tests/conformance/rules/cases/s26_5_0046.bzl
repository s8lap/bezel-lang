//! case: s26_5_0046
//! doc: spec
//! section: 26.5
//! title: an extension GetField called 100 times on a literal receiver yields its result every time
//! rule: If the receiver's type declares no instance member of that name, the compiler searches the current module and all imported modules
//! cites: 26.5
//! expect: run
//! output: 490
module Conf;

extension string
  public method GetField(d: char, n: int): string => "EXT" + n.ToString();
end;

public class Program
  public static method Main()
    var i := 0;
    var t := 0;
    while i < 100 do
      var r := "a,b".GetField(',', i);
      t := t + r.Length;
      i := i + 1;
    end;
    Console.WriteLine(t);
  end Main;
end Program;
