//! case: l3_2_0009
//! doc: stdlib
//! section: 3.2
//! title: GetByteCount over a string returned by a call counts the UTF-8 bytes
//! rule: Stdlib §3.2: `GetByteCount(s: string): int`, and "`UTF8` ...: bytes are the UTF-8 form of the string's code
//!   points": `"aé"` is U+0061 (1 byte) and U+00E9 (2 bytes), so the count is 3, for a string that is a call result.
//! cites: 3.2
//! expect: run
//! stdout: 3
module Conf;

public class Program
  static method Make(): string return "aé"; end Make;

  public static method Main()
    Console.WriteLine(Encoding.UTF8.GetByteCount(Make()));
  end Main;
end Program;
