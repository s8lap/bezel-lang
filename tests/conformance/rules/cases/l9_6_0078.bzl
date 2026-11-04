//! case: l9_6_0078
//! doc: stdlib
//! section: 9.6
//! title: Trim with a single char argument matches no declared form
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: reject
//! diagnostic: Trim
//! issue: 7521
//! notes: A char is not an IEnumerable<char>; no Trim(char) overload is declared.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("ab".Trim('a'));
  end Main;
end Program;
