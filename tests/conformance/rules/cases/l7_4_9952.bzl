//! case: l7_4_9952
//! doc: stdlib
//! section: 7.4
//! title: Console.In.ReadLine() and Read() at end of input answer null and -1 in a 300x loop
//! rule: null at EOF
//! cites: 7.4, 2.1, 28.9
//! expect: run
//! output: -300
//! notes: Pins #9260 (the string ReadLine returns is a fresh plus-one value the scope end releases; the
//!   leak half needs a sanitizer leg the corpus runner lacks). Empty stdin: ReadLine() is null, so
//!   Keep is never called; Read() is -1 each pass: total = 300 * -1 = -300.
module Conf;
public class Program
  static method Keep(s: string): int
    return s.Length;
  end Keep;
  public static method Main(): int
    var total := 0;
    var i := 0;
    while i < 300 do
      var l := Console.In.ReadLine();
      if l <> null then total := total + Keep(l); end;
      var c := Console.In.Read();
      total := total + c;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
