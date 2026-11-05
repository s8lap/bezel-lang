//! case: l4_19_1803
//! doc: stdlib
//! section: 4.19
//! title: ToArray on an annotated FrozenArray<string> copies out string elements
//! rule: public method ToArray(): owned Array<T>;
//! cites: 26.5
//! expect: run
//! output: 200
//! issue: 8104
module Conf;

class Program
  public static method Main()
    var total := 0;
    for var i in 0..<50 do
      var a: Array<string> := ["ab", "c"];
      using var g: FrozenArray<string> := FrozenArray<string>.From(a);
      using var b := g.ToArray();
      total := total + b.Count + b[0].Length;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
