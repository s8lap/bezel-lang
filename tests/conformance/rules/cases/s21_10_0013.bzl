//! case: s21_10_0013
//! doc: spec
//! section: 21.10
//! title: a local record gets §9.2 auto-equality like any other record
//! rule: The local type is an ordinary nominal record — every §9 rule applies
//!   unchanged; auto-equality (§9.2) is computed by its existing rules from
//!   the declaration.
//! cites: 21.10, 9.2
//! expect: run
//! output: True
//!   False
module Conf;

public class Program
  public static method Main()
    record Tag(Name: string, Rank: int);
    var a := new Tag("x", 1);
    var b := new Tag("x", 1);
    var c := new Tag("x", 2);
    Console.WriteLine(a = b);
    Console.WriteLine(a = c);
  end Main;
end Program;
