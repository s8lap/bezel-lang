//! case: s28_3_11_7203
//! doc: spec
//! section: 28.3.11
//! title: a string interpolated or concatenated from an unbound dictionary
//!   temporary's index read is intact
//! rule: is disposed at the **end of the full expression** in which it is
//!   produced, after every use of it within that expression has completed
//! cites: 28.3, 28.9, 22.2
//! expect: run
//! output: 1800
//!   <vaa>vaa!
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
    var f := lambda() => $"<{P.MakeDict()["aa"]}>";
    var g := lambda() => P.MakeDict()["aa"] + "!";
    while i < 200 do
      n := n + f().Length + g().Length;
      i := i + 1;
    end while;
    Console.WriteLine(n);
    Console.WriteLine(f() + g());
  end Main;
end Program;
