//! case: s22_12_8004
//! doc: spec
//! section: 22.12
//! title: ReferenceEquals named through a class inside a lambda body compares identity
//! rule: A static member declared on a base class is a member of every class derived from it
//! cites: 22.12, 8.1, 27.3
//! expect: run
//! stdout: 300
//! notes: Pins #8795 inside a lambda body.
module Conf;
public class K end K;
public class Program
  public static method Main(): int
    var k := new K();
    var k2 := new K();
    var f := lambda(a: K, b: K) => K.ReferenceEquals(a, b);
    var n := 0;
    for var i in 0..<300 do
      if f(k, k) then n := n + 1; end;
      if f(k, k2) then n := n + 1000; end;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
