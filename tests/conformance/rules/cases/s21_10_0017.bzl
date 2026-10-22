//! case: s21_10_0017
//! doc: spec
//! section: 21.10
//! title: a local enum declared as a statement compares its members like an enum at namespace scope
//! rule: "The local type is an **ordinary nominal record or enum** — every §9 (records) and §10 (enums) rule applies unchanged"
//! cites: 10
//! expect: run
//! output: True
//!   False
module Conf;

public class Program
  public static method Main()
    enum Level
      Low, High;
    end Level;
    var lv := Level.High;
    Console.WriteLine(lv = Level.High);
    Console.WriteLine(lv = Level.Low);
  end Main;
end Program;
