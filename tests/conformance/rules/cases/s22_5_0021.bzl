//! case: s22_5_0021
//! doc: spec
//! section: 22.5
//! title: a string member's supplied arguments run once each, in call-site textual order
//! rule: stdlib §9.6 declares `Remove(startIndex: int, length: int): string`. §22.5,
//!   Evaluation order: a call's supplied argument expressions are evaluated in
//!   call-site textual order - left to right as written, for positional and named
//!   arguments alike; naming an argument changes which parameter it binds to,
//!   never when it runs. The `length` expression is written first, so it runs
//!   first, and it runs once - the call reads "len" once and "start" once.
//! cites: 22.5, 9.6
//! expect: run
//! output: len
//!   start
//!   abdef
//! issue: 7181
module Conf;

public class Marker
  public method Next(tag: string, value: int): int
    Console.WriteLine(tag);
    return value;
  end Next;
end Marker;

public class Program
  public static method Main()
    var m := new Marker();
    var s := "abcdef";
    Console.WriteLine(s.Remove(length := m.Next("len", 1), startIndex := m.Next("start", 2)));
  end Main;
end Program;
