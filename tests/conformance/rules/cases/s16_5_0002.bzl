//! case: s16_5_0002
//! doc: spec
//! section: 16.5
//! title: iterating an open-ended range stored in a variable raises InvalidOperationException at runtime
//! rule: "For ranges stored in variables, the check is deferred to runtime: `var r:
//!   Range<int> := (if someCondition then 1..10 else 10..); for var i in r do ...
//!   end; // InvalidOperationException if r is open-ended`."
//! cites: 16.5, 16.3
//! expect: run
//! stdout: invalid
module Conf;

public class Program
  public static method Main()
    var someCondition: bool := false;
    var r: Range<int> := (if someCondition then 1..10 else 10..);
    try
      for var i in r do
        Console.WriteLine(i);
      end for;
    catch
      var ex: InvalidOperationException => Console.WriteLine("invalid")
    end try;
  end Main;
end Program;
