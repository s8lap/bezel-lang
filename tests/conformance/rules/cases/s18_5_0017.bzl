//! case: s18_5_0017
//! doc: spec
//! section: 18.5
//! title: present and null nullable holes in a loop render the value's ToString and the text null
//! rule: `ToString` is invoked implicitly inside string interpolation via
//!   virtual dispatch for present values; a `null` value of a nullable-typed
//!   hole renders as the text `null` with no dispatch.
//! cites: 18.5, 8.5, 4.6
//! expect: run
//! output: xnull5null
//!   hi/null
//!   100
module Conf;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<100 do
      var s1: string? := "x";
      var s0: string? := null;
      var i1: int? := 5;
      var i0: int? := null;
      var a: StringBuilder? := new StringBuilder("hi");
      var b: StringBuilder? := null;
      var x := $"{s1}{s0}{i1}{i0}";
      var s := $"{a}/{b}";
      if i = 0 then
        Console.WriteLine(x);
        Console.WriteLine(s);
      end;
      if s = "hi/null" and x = "xnull5null" then
        total := total + 1;
      end;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
