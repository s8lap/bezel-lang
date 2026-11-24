//! case: s27_5_7917
//! doc: spec
//! section: 27.5
//! title: a lambda that writes the captured local in a loop that also writes it is visible
//!   both ways
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5
//! expect: run
//! stdout: 248
//! issue: 9004
module M; public class C
  public static method Main(): int
    var k := 3; var s := 0;
    for var i in 0..<5 do k := k + i; var h := lambda() => do k := k + 10; return k; end; s := s + h(); end;
    Console.WriteLine(s + k);
    return 0;
  end Main;
end C;
