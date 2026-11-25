//! case: l2_1_9702
//! doc: stdlib
//! section: 2.1
//! title: Console.ReadLine() ?? "" at end of input yields the empty string, 200x
//! rule: null at EOF
//! cites: 2.1, 18.5, 28.3
//! expect: run
//! output: 400
//! notes: Pins #9213 (pick 714c9ea99): the string Console.ReadLine() returns is a fresh plus-one
//!   production that the scope end must release; the leak half needs the sanitizer leg the corpus
//!   runner lacks, so this case pins the observable half over a 200x loop. The corpus runner supplies
//!   an empty stdin, so every ReadLine() is at end of input and yields null. The coalesce takes the
//!   empty string, so framed is [] with Length 2: 200 * 2 = 400.
module Conf;

public class Program
  public static method Main(): int
    var total := 0;
    var i := 0;
    while i < 200 do
      var s := Console.ReadLine() ?? "";
      var framed := "[" + s + "]";
      total := total + framed.Length;
      i := i + 1;
    end while;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
