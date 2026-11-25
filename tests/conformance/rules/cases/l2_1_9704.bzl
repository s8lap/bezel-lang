//! case: l2_1_9704
//! doc: stdlib
//! section: 2.1
//! title: Console.ReadLine() returned from a method is null at end of input, 200x
//! rule: null at EOF
//! cites: 2.1, 28.3, 28.9
//! expect: run
//! output: 200
//! notes: Pins #9213 (pick 714c9ea99): the string Console.ReadLine() returns is a fresh plus-one
//!   production that the scope end must release; the leak half needs the sanitizer leg the corpus
//!   runner lacks, so this case pins the observable half over a 200x loop. The corpus runner supplies
//!   an empty stdin, so every ReadLine() is at end of input and yields null. ReadOne returns the null
//!   ReadLine() result to the caller, which counts it: 200.
module Conf;

public class Program
  public static method ReadOne(): string?
    return Console.ReadLine();
  end ReadOne;
  public static method Main(): int
    var total := 0;
    var i := 0;
    while i < 200 do
      var s := ReadOne();
      if s = null then
        total := total + 1;
      end if;
      i := i + 1;
    end while;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
