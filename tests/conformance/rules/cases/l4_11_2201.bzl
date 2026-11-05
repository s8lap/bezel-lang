//! case: l4_11_2201
//! doc: stdlib
//! section: 4.11
//! title: Stack.TryPeek into a pre-declared holding string? local reads the
//!   top element
//! rule: public method TryPeek(out item: T?): bool;
//! cites: 1
//! expect: run
//! stdout: 600
//! notes: Trace: 200 iterations, each hit adds the 3-character stored
//!   string ("a40".."a49"), so 600. Aborts with a refcount
//!   over-release on the pre-fix compiler (parent of a29fb10c), passes
//!   on the staging tip.
module Conf;

public class Program
  public static method Main()
    var tot := 0;
    var i := 0;
    while i < 200 do
      var c := new Stack<string>();
      c.Push("a" + (40 + i mod 10).ToString());
      var v: string? := "init" + i.ToString();
      if c.TryPeek(out v) then tot := tot + (v ?? "").Length; end;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
