//! case: s26_5_1804
//! doc: spec
//! section: 26.5
//! title: an IEnumerable<string> extension is reached on a FrozenArray<string> receiver
//! rule: for extension blocks on `x`'s type, base types, or implemented interfaces
//! cites: 4.19
//! expect: run
//! output: 150
//! issue: 8103
module Conf;

extension IEnumerable<string>
  public method Joined(): string
    var s := "";
    for var x in this do s := s + x; end;
    return s;
  end Joined;
end;
class Program
  public static method Main()
    var total := 0;
    for var i in 0..<50 do
      var a: Array<string> := ["ab", "c"];
      using var g: FrozenArray<string> := FrozenArray<string>.From(a);
      total := total + g.Joined().Length;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
