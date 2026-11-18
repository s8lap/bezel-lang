//! case: s28_3_11_7206
//! doc: spec
//! section: 28.3.11
//! title: a lambda reading a named bound dictionary does not dispose it, so
//!   it stays readable afterwards
//! rule: that is not bound to any name, not `move`d
//! cites: 28.3, 22.2
//! expect: run
//! output: 800
//!   vaa
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<string, string>();
    d.Add("aa", "va" + "a");
    var n := 0;
    var i := 0;
    var f := lambda() => d["aa"];
    while i < 200 do
      n := n + f().Length + d.Count;
      i := i + 1;
    end while;
    Console.WriteLine(n);
    Console.WriteLine(d["aa"]);
  end Main;
end Program;
