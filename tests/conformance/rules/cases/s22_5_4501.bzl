//! case: s22_5_4501
//! doc: spec
//! section: 22.5
//! title: named arguments of int.Parse run in call-site textual order whichever order
//!   the parameters declare
//! rule: naming an argument changes which parameter it binds to, never when it runs
//! cites: 22.6
//! expect: run
//! output: radix
//!   s
//!   255
//!   s2
//!   radix2
//!   255
//! issue: 7602
module Conf;

class C
  public static method N(tag: string, v: int): int
    Console.WriteLine(tag);
    return v;
  end N;
  public static method S(tag: string, v: string): string
    Console.WriteLine(tag);
    return v;
  end S;
  public static method Main(): int
    Console.WriteLine(int.Parse(radix := N("radix", 16), s := S("s", "ff")));
    Console.WriteLine(int.Parse(s := S("s2", "ff"), radix := N("radix2", 16)));
    return 0;
  end Main;
end C;
