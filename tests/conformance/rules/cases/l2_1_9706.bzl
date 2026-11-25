//! case: l2_1_9706
//! doc: stdlib
//! section: 2.1
//! title: a local reassigned from Console.ReadLine() stays null at end of input, 200x
//! rule: null at EOF
//! cites: 2.1, 28.3, 28.9
//! expect: run
//! output: 200
//! notes: Pins #9213 (pick 714c9ea99): the string Console.ReadLine() returns is a fresh plus-one
//!   production that the scope end must release; the leak half needs the sanitizer leg the corpus
//!   runner lacks, so this case pins the observable half over a 200x loop. The corpus runner supplies
//!   an empty stdin, so every ReadLine() is at end of input and yields null. The local is first bound
//!   to the null result (nothing added), then reassigned from ReadLine() each iteration: each takes
//!   the null branch, 200.
module Conf;

public class Program
  public static method Main(): int
    var total := 0;
    var s := Console.ReadLine();
    if s <> null then
      total := total + 1000;
    end if;
    var i := 0;
    while i < 200 do
      s := Console.ReadLine();
      if s = null then
        total := total + 1;
      end if;
      i := i + 1;
    end while;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
