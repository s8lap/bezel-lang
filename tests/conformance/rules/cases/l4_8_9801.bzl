//! case: l4_8_9801
//! doc: stdlib
//! section: 4.8
//! title: A declined TryAdd leaves the dictionary and the source choice unchanged, 200x
//! rule: false and no mutation if the key is present
//! cites: 4.8, 28.11, 28.9
//! expect: run
//! output: 1000
//! notes: Pins #9197/#9254 (picks 0c6b7d2da, 201f50859): a Dictionary<string, Shape> slot copies a choice
//!   whose payload is a string, taking its own reference on the string; removing the copy (Clear,
//!   Remove, a declined TryAdd) must leave the source's string readable. The leak half needs the
//!   sanitizer leg the corpus runner lacks; this case pins the observable half over a 200x loop. The
//!   second TryAdd of the present key a returns false (nothing mutated), so no 100 is added; s reads
//!   Length 5 each iteration: 1000.
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
      if d.TryAdd("a", s) then total := total + 100; end;
      total := total + (case s of Named(n) => n.Length, Circle(r) => 0);
      i := i + 1;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
