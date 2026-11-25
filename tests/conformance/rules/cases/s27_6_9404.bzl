//! case: s27_6_9404
//! doc: spec
//! section: 27.6
//! title: bare method group locals select the conditional's branch and call a unit method
//! rule: Method references work for static methods, instance methods (the instance is captured at
//!   formation
//! cites: 27.6, 27.7, 20
//! expect: run
//! output: 7
//!   4800
module Conf;

public class Program
  static method Say(i: int)
    Console.WriteLine(i);
  end Say;
  static method P(i: int): int => i + 10;
  static method Q(i: int): int => i + 20;
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 300 do
      var c := (i / 2) * 2 = i;
      var f := (if c then P else Q);
      t := t + f(1);
      var s := Say;
      if i = 299 then
        s(7);
      end if;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
