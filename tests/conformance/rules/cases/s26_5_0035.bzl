//! case: s26_5_0035
//! doc: spec
//! section: 26.5
//! title: an extension Repeat on string binds an instance call while the static String.Repeat is unaffected
//! rule: If the receiver's type declares no instance member of that name, the compiler searches the current module and all imported modules ... Extensions are resolved on the compile-time type of the receiver, not the runtime type.
//! cites: 26.1, 26.3
//! expect: run
//! output: EXT
//!   EXT
//!   abcabc
//!   xxx
module Conf;
extension string public method Repeat(n: int): string => "EXT"; end;
public class Program
  public static method Main(): int
    var s := "abc";
    Console.WriteLine(s.Repeat(2));
    Console.WriteLine("abc".Repeat(2));
    Console.WriteLine(String.Repeat(s, 2));
    Console.WriteLine(string.Repeat('x', 3));
    return 0;
  end Main;
end Program;
