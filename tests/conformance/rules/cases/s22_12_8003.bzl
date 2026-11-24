//! case: s22_12_8003
//! doc: spec
//! section: 22.12
//! title: ReferenceEquals named through a derived class name compares identity
//! rule: A static member declared on a base class is a member of every class derived from it
//! cites: 22.12, 8.1
//! expect: run
//! stdout: 300
//! notes: Pins #8795 on a derived and a base class name in a loop.
module Conf;
public class B end B;
public class D: B end D;
public class Program
  public static method Main(): int
    var d := new D();
    var e := new D();
    var n := 0;
    for var i in 0..<300 do
      if D.ReferenceEquals(d, d) then n := n + 1; end;
      if B.ReferenceEquals(d, e) then n := n + 1000; end;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
