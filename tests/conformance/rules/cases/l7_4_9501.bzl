//! case: l7_4_9501
//! doc: stdlib
//! section: 7.4
//! title: a string bound to Console.In.ReadToEnd() is reassigned, aliased and overwritten in a loop
//! rule: public method ReadToEnd(): string;
//! cites: 7.4, 9.6
//! expect: run
//! output: 200
//! notes: Pins #9199 (pick 702fb3d9c): a string bound to Console.In.ReadToEnd() is a plus-one string that
//!   the scope end must release; the leak half needs the ASan leg the corpus runner lacks, so this
//!   case pins the observable half over a 200x loop. The corpus runner supplies an empty stdin, so
//!   every ReadToEnd() is the empty string (no text remains). Per iteration: s.Length is 1 (the
//!   literal) and t.Length is 0 (the empty read): 200.
module Conf;

public class Program
  public static method Main(): int
    var total := 0;
    var i := 0;
    while i < 200 do
      var s := Console.In.ReadToEnd();
      s := Console.In.ReadToEnd();
      var t := s;
      s := "x";
      total := total + s.Length + t.Length;
      i := i + 1;
    end while;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
