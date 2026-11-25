//! case: s28_11_9804
//! doc: spec
//! section: 28.11
//! title: A choice stored through a method parameter into a Dictionary stays readable, 200x
//! rule: copies the value in
//! cites: 28.11, 28.9
//! expect: run
//! output: 1200
//! notes: Pins #9197/#9254 (picks 0c6b7d2da, 201f50859): a Dictionary<string, Shape> slot copies a choice
//!   whose payload is a string, taking its own reference on the string; removing the copy (Clear,
//!   Remove, a declined TryAdd) must leave the source's string readable. The leak half needs the
//!   sanitizer leg the corpus runner lacks; this case pins the observable half over a 200x loop. Put
//!   copies v into a local Dictionary that dies at the method's end; the caller's s still reads:
//!   Count 1 plus Length 5 = 6 per iteration, 1200.
module Conf;

public choice Shape Circle(r: int), Named(n: string); end Shape;

public class Holder
  public method Put(v: Shape): int
    var d := new Dictionary<string, Shape>();
    d["a"] := v;
    return d.Count;
  end Put;
end Holder;

public class Program
  public static method Main()
    var i := 0;
    var n := 0;
    while i < 200 do
      using var h := new Holder();
      var s := Shape.Named("p" + (i + 1000).ToString());
      n := n + h.Put(s);
      n := n + (case s of Named(k) => k.Length, Circle(r) => 0);
      i := i + 1;
    end;
    Console.WriteLine(n);
  end Main;
end Program;
