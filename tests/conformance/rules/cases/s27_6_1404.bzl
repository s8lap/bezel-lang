//! case: s27_6_1404
//! doc: spec
//! section: 27.6
//! title: a bound reference over a string local survives being returned out of the local's scope
//! rule: (the receiver's, or the sources of the borrows a value-shaped receiver carries)
//! cites: 27.6, 28.3
//! expect: run
//! stdout: 110
module Conf;

extension string
  public method Shout(): string => this + "!";
end extension;

public class Program
  static method Mk(i: int): string => "abc" + i.ToString();

  static method Make(i: int): () -> string
    var s := Mk(i);
    var f: () -> string := s.Shout;
    return f;
  end Make;

  public static method Main()
    var n := 0;
    for var i in 0..<20 do
      var g := Make(i);
      n += g().Length;
    end;
    Console.WriteLine(n);
  end Main;
end Program;
