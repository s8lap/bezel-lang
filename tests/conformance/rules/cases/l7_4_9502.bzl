//! case: l7_4_9502
//! doc: stdlib
//! section: 7.4
//! title: string members and operators over Console.In.ReadToEnd() as an unbound operand in a loop
//! rule: public method ReadToEnd(): string;
//! cites: 7.4, 9.6, 18.5
//! expect: run
//! output: 701
//! notes: Pins #9199 (pick 702fb3d9c): a string bound to Console.In.ReadToEnd() is a plus-one string that
//!   the scope end must release; the leak half needs the ASan leg the corpus runner lacks, so this
//!   case pins the observable half over a 200x loop. The corpus runner supplies an empty stdin, so
//!   every ReadToEnd() is the empty string (no text remains). Per iteration a=0, b=0, c="[]" (2),
//!   d="z" (1), e=0, f=0, and cond is "q" (1) for i = 0..100 and the empty read (0) for i = 101..199:
//!   101*4 + 99*3 = 701.
module Conf;

public class Program
  public static method Main(): int
    var total := 0;
    var i := 0;
    while i < 200 do
      var a := Console.In.ReadToEnd().Length;
      var b := Console.In.ReadToEnd().Trim();
      var c := $"[{Console.In.ReadToEnd()}]";
      var d := Console.In.ReadToEnd() + "z";
      var s := Console.In.ReadToEnd();
      var e := s.ToUpper();
      var f := s + s;
      var cond := (if i > 100 then Console.In.ReadToEnd() else "q");
      total := total + a + b.Length + c.Length + d.Length + e.Length + f.Length + cond.Length;
      i := i + 1;
    end while;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
