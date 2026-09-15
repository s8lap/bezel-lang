//! case: s35_7_0001
//! doc: spec
//! section: 35.7
//! title: a value-like choice is auto-hashable and its `=` compares active case and payloads
//! rule: A value-like choice is auto-hashable when every payload field of every case is
//!   an admissible component type. The generated = is true exactly when both
//!   operands have the same active case and every corresponding payload compares
//!   equal by its type's =.
//! cites: 35.7, 9.2
//! expect: run
//! output: True
//!   False
module Conf;

public choice Status
  Active(code: int),
  Inactive;
end Status;

public class Program
  public static method Main()
    var a := Status.Active(1);
    var b := Status.Active(1);
    var c := Status.Inactive;
    Console.WriteLine(a = b);   // same case, equal payload
    Console.WriteLine(a = c);   // different active case
  end Main;
end Program;
