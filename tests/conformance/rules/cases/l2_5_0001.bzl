//! case: l2_5_0001
//! doc: stdlib
//! section: 2.5
//! title: DateTime's no-format ToString renders ISO 8601
//! rule: The no-argument DateTime.ToString() renders the round-trippable ISO
//!   8601 form yyyy-MM-ddTHH:mm:ss.
//! expect: run
//! stdout: 2024-03-15T09:30:05
module Conf;

public class Program
  public static method Main()
    var d := new DateTime(2024, 3, 15, 9, 30, 5);
    Console.WriteLine(d.ToString());
  end Main;
end Program;
