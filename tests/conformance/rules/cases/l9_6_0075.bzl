//! case: l9_6_0075
//! doc: stdlib
//! section: 9.6
//! title: Trim with a nullable Set<char>? parameter that is not narrowed is rejected
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 7.1
//! expect: reject
//! diagnostic: Trim
//! issue: 7521
//! notes: Twin: the Set<char> row. The nullable source is rejected before overload applicability.
module Conf;

public class Program
  public static method Pick(s: Set<char>?): string
    return "ab".Trim(s);
  end Pick;

  public static method Main()
    Console.WriteLine(Pick(null));
  end Main;
end Program;
