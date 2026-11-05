//! case: s27_3_2002
//! doc: spec
//! section: 27.3
//! title: a lambda returning an enum value of the slot's own enum is
//!   admitted
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1, 10.5
//! expect: run
//! stdout: True
module Conf;

enum Pri
  Low, High;
end Pri;

public class Program
  public static method Main()
    var f: () -> Pri := lambda() => Pri.High;
    Console.WriteLine(f() = Pri.High);
  end Main;
end Program;
