//! case: l2_1_9705
//! doc: stdlib
//! section: 2.1
//! title: Console.ReadLine() read into a local that returns early is null at end of input, 200x
//! rule: null at EOF
//! cites: 2.1, 28.3, 28.9
//! expect: run
//! output: 400
//! notes: Pins #9213 (pick 714c9ea99): the string Console.ReadLine() returns is a fresh plus-one
//!   production that the scope end must release; the leak half needs the sanitizer leg the corpus
//!   runner lacks, so this case pins the observable half over a 200x loop. The corpus runner supplies
//!   an empty stdin, so every ReadLine() is at end of input and yields null. Probe takes the early
//!   return 2 on the null result every call: 200 * 2 = 400.
module Conf;

public class Program
  public static method Probe(): int
    var s := Console.ReadLine();
    if s = null then
      return 2;
    end if;
    if s.Length > 3 then
      return s.Length;
    end if;
    return 0;
  end Probe;
  public static method Main(): int
    var total := 0;
    var i := 0;
    while i < 200 do
      total := total + Probe();
      i := i + 1;
    end while;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
