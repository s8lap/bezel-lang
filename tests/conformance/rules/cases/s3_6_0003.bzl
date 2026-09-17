//! case: s3_6_0003
//! doc: spec
//! section: 3.6
//! title: default of the nullable form of a reference type is null
//! rule: defaultable classifies types for which default(T) is defined: every
//!   nullable type T?.
//! cites: 3.6, 17.11, 18.5
//! expect: run
//! stdout: [null]
module Conf;

public class Program
  public static method Main()
    var s: string? := default(string?);
    Console.WriteLine($"[{s}]");
  end Main;
end Program;
