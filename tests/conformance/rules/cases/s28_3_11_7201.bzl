//! case: s28_3_11_7201
//! doc: spec
//! section: 28.3.11
//! title: a string indexed out of an unbound dictionary temporary in a
//!   lambda survives its disposal
//! rule: is disposed at the **end of the full expression** in which it is
//!   produced, after every use of it within that expression has completed
//! cites: 28.3, 28.9, 22.2
//! expect: run
//! output: 600
//!   vaa
module Conf;

public class P
  public static method MakeDict(): owned Dictionary<string, string>
    var d: Dictionary<string, string> := new Dictionary<string, string>();
    d.Add("aa", "va" + "a");
    return d;
  end MakeDict;
end P;

public class Program
  public static method Main()
    var n := 0;
    var i := 0;
    var f := lambda() => P.MakeDict()["aa"];
    while i < 200 do
      var s := f();
      n := n + s.Length;
      i := i + 1;
    end while;
    Console.WriteLine(n);
    Console.WriteLine(f());
  end Main;
end Program;
