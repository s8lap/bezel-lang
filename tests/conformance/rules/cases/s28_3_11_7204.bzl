//! case: s28_3_11_7204
//! doc: spec
//! section: 28.3.11
//! title: a nested expression-bodied lambda indexing an unbound dictionary
//!   temporary returns an intact string
//! rule: is disposed at the **end of the full expression** in which it is
//!   produced, after every use of it within that expression has completed
//! cites: 28.3, 22.2
//! expect: run
//! stdout: 600
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
    var outer := lambda() => lambda() => P.MakeDict()["aa"];
    while i < 200 do
      var inner := outer();
      var s := inner();
      n := n + s.Length;
      i := i + 1;
    end while;
    Console.WriteLine(n);
  end Main;
end Program;
