//! case: s17_11_0018
//! doc: spec
//! section: 17.11
//! title: default of a record whose reference field is nullable yields null in that field
//! rule: "Records all of whose declared fields are defaultable ... the record
//!   value with each field at its zero value ... Reference types: `null` if the
//!   location is nullable."
//! cites: 17.11, 9.1, 7.1
//! expect: run
//! stdout: True
module Conf;

public record Named(Label: string?);

public class Program
  public static method Main()
    var n := default(Named);
    Console.WriteLine(n.Label = null);
  end Main;
end Program;
