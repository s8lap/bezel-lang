//! case: s35_7_0002
//! doc: spec
//! section: 35.7
//! title: the generated ToString renders TypeName.CaseName with its payloads
//! rule: The generated ToString produces TypeName.CaseName(field1: value1, ...), and
//!   TypeName.CaseName for a payload-free case.
//! expect: run
//! output: Status.Active(code: 7)
//!   Status.Inactive
module Conf;

public choice Status
  Active(code: int),
  Inactive;
end Status;

public class Program
  public static method Main()
    var a := Status.Active(7);
    var b := Status.Inactive;
    Console.WriteLine(a.ToString());
    Console.WriteLine(b.ToString());
  end Main;
end Program;
