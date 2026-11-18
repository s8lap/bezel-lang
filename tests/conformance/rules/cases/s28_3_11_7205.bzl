//! case: s28_3_11_7205
//! doc: spec
//! section: 28.3.11
//! title: the block-bodied twin of a lambda over an unbound dictionary
//!   temporary yields the same value
//! rule: is disposed at the **end of the full expression** in which it is
//!   produced, after every use of it within that expression has completed
//! cites: 28.3, 22.2
//! expect: run
//! stdout: 303
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
    var f := lambda() => do
      var x := P.MakeDict()["aa"].Length;
      return x;
    end;
    while i < 101 do
      n := n + f();
      i := i + 1;
    end while;
    Console.WriteLine(n);
  end Main;
end Program;
