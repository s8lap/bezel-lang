//! case: s28_11_9803
//! doc: spec
//! section: 28.11
//! title: Both choices of an if-expression stored into a Dictionary survive Clear(), 200x
//! rule: `Clear()` | removes all | removes all | removes all
//! cites: 28.11, 28.9
//! expect: run
//! output: 2000
//! notes: Pins #9197/#9254 (picks 0c6b7d2da, 201f50859): a Dictionary<string, Shape> slot copies a choice
//!   whose payload is a string, taking its own reference on the string; removing the copy (Clear,
//!   Remove, a declined TryAdd) must leave the source's string readable. The leak half needs the
//!   sanitizer leg the corpus runner lacks; this case pins the observable half over a 200x loop.
//!   Whichever branch is stored, Clear removes a copy; s and t both stay readable: 5 + 5 per
//!   iteration, 2000.
module Conf;

public choice Shape Circle(r: int), Named(n: string); end Shape;

public class Program
  public static method Main()
    var i := 0;
    var total := 0;
    while i < 200 do
      var s := Shape.Named("p" + (i + 1000).ToString());
      var t := Shape.Named("x" + (i + 1000).ToString());
      var d := new Dictionary<string, Shape>();
      d["a"] := (if i > 5 then s else t);
      d.Clear();
      total := total + (case s of Named(n) => n.Length, Circle(r) => 0) + (case t of Named(n) => n.Length, Circle(r) => 0);
      i := i + 1;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
