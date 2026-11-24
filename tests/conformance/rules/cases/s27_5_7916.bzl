//! case: s27_5_7916
//! doc: spec
//! section: 27.5
//! title: a lambda formed after a conditional break in a for loop reads the local the loop
//!   updated before it
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5
//! expect: run
//! stdout: 77
//! issue: 9004
module M; public class C
  public static method Main(): int
    var k := 3; var s := 0;
    for var i in 0..<10 do
      k := k + i;
      if i = 6 then break; end;
      var h := lambda() => k; s := s + h();
    end;
    Console.WriteLine(s + k);
    return 0;
  end Main;
end C;
