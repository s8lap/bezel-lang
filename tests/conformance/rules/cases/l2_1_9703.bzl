//! case: l2_1_9703
//! doc: stdlib
//! section: 2.1
//! title: Console.ReadLine() passed as a string? argument is null at end of input, 200x
//! rule: null at EOF
//! cites: 2.1, 28.3, 28.9
//! expect: run
//! output: -200
//! notes: Pins #9213 (pick 714c9ea99): the string Console.ReadLine() returns is a fresh plus-one
//!   production that the scope end must release; the leak half needs the sanitizer leg the corpus
//!   runner lacks, so this case pins the observable half over a 200x loop. The corpus runner supplies
//!   an empty stdin, so every ReadLine() is at end of input and yields null. Take returns -1 for a
//!   null argument: 200 * -1 = -200.
module Conf;

public class Program
  public static method Take(x: string?): int
    if x <> null then
      return x.Length;
    end if;
    return -1;
  end Take;
  public static method Main(): int
    var total := 0;
    var i := 0;
    while i < 200 do
      var n := Take(Console.ReadLine());
      total := total + n;
      i := i + 1;
    end while;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
