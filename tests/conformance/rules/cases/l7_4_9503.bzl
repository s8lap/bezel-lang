//! case: l7_4_9503
//! doc: stdlib
//! section: 7.4
//! title: a string bound to Console.In.ReadToEnd() is returned from a method, bound or direct
//! rule: public method ReadToEnd(): string;
//! cites: 7.4
//! expect: run
//! output: 200
//! notes: Pins #9199 (pick 702fb3d9c): a string bound to Console.In.ReadToEnd() is a plus-one string that
//!   the scope end must release; the leak half needs the ASan leg the corpus runner lacks, so this
//!   case pins the observable half over a 200x loop. The corpus runner supplies an empty stdin, so
//!   every ReadToEnd() is the empty string (no text remains). Each iteration adds 0 + 0 + 1: 200.
module Conf;

public class Program
  static method Bound(): string
    var s := Console.In.ReadToEnd();
    return s;
  end Bound;

  static method Direct(): string
    return Console.In.ReadToEnd();
  end Direct;

  public static method Main(): int
    var total := 0;
    var i := 0;
    while i < 200 do
      var t := Program.Bound();
      var u := Program.Direct();
      total := total + t.Length + u.Length + 1;
      i := i + 1;
    end while;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
