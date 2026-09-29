//! case: s17_6_0012
//! doc: spec
//! section: 17.6
//! title: ++ on a char target applies the code-point rules of the conversion
//! rule: "For a `char` target the §5.2 code-point rules apply (`c++` is
//!   `c := char(int(c) + 1)`)."
//! cites: 17.6, 6.6, 5.2
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var c: char := 'a';
    c++;
    Console.WriteLine(c = 'b');
  end Main;
end Program;
