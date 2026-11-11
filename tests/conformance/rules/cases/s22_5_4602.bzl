//! case: s22_5_4602
//! doc: spec
//! section: 22.5
//! title: naming an argument changes which parameter it binds, never when it runs
//! rule: naming an argument changes which parameter it binds to, never when it runs
//! cites: 9.1
//! expect: run
//! output: radix
//!   s
//!   255
//!   s2
//!   radix2
//!   255
//! issue: 7602
module Conf;

public class Program
  public static method N(tag: string, v: int): int
    Console.WriteLine(tag);
    return v;
  end N;
  public static method S(tag: string, v: string): string
    Console.WriteLine(tag);
    return v;
  end S;
  public static method Main()
    Console.WriteLine(int.Parse(radix := N("radix", 16), s := S("s", "ff")));
    Console.WriteLine(int.Parse(s := S("s2", "ff"), radix := N("radix2", 16)));
  end Main;
end Program;
