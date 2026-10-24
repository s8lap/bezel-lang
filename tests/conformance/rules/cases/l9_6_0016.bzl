//! case: l9_6_0016
//! doc: stdlib
//! section: 9.6
//! title: string.PadCenter has no static form
//! rule: public pure method PadCenter(totalWidth: int, paddingChar: char := ' '): string;
//!   — declared as an instance member of `String`, with no static form, so a
//!   static reference to it names no declared member and is a compile error.
//! cites: 9.6, 22.6, 3.4
//! expect: reject
//! diagnostic: PadCenter
module Conf;

public class Program
  public static method Main(): int
    var t := string.PadCenter(5);
    Console.WriteLine(t.Length);
    return 0;
  end Main;
end Program;
