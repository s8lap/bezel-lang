//! case: s28_11_9802
//! doc: spec
//! section: 28.11
//! title: A choice local added with TryAdd then removed from a Dictionary is still readable, 200x
//! rule: removes the copy | removes the reference | removes the borrow
//! cites: 28.11, 28.9, 4.8
//! expect: run
//! output: 1000
//! notes: Pins #9197/#9254 (picks 0c6b7d2da, 201f50859): a Dictionary<string, Shape> slot copies a choice
//!   whose payload is a string, taking its own reference on the string; removing the copy (Clear,
//!   Remove, a declined TryAdd) must leave the source's string readable. The leak half needs the
//!   sanitizer leg the corpus runner lacks; this case pins the observable half over a 200x loop.
//!   TryAdd copies s in, Remove removes the copy; s still reads p1000.. of Length 5.
module Conf;

public choice Shape Circle(r: int), Named(n: string); end Shape;

public class Program
  public static method Main()
    var i := 0;
    var total := 0;
    while i < 200 do
      var s := Shape.Named("p" + (i + 1000).ToString());
      var d := new Dictionary<string, Shape>();
      _ := d.TryAdd("a", s);
      _ := d.Remove("a");
      total := total + (case s of Named(n) => n.Length, Circle(r) => 0);
      i := i + 1;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
