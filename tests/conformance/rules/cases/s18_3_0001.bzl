//! case: s18_3_0001
//! doc: spec
//! section: 18.3
//! title: a full-code-point char escape resolves to its numeric code point
//! rule: "`'\U0001F600'` // 8 hex digits, full code point range" is an admitted
//!   char literal form; converting it to `int` yields the code point's decimal
//!   value (stdlib: "`I(c)` for `char` `c` ... converts the code-point value").
//! cites: 18.3
//! expect: run
//! stdout: 128512
module Conf;

public class Program
  public static method Main()
    var c := '\U0001F600';
    Console.WriteLine(int(c));
  end Main;
end Program;
