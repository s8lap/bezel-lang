//! case: s27_5_8103
//! doc: spec
//! section: 27.5
//! title: a closure that mutates the captured loop-carried local is visible to the
//!  enclosing method
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 62
//! notes: Pins #9004 (instance-method twin). Per pass k += i, then the closure adds one and
//!  returns k: k=2 gives 3 (s=3); k=4 gives 5 (s=8); k=7 gives 8 (s=16); k=11 gives 12
//!  (s=28); k=16 gives 17 (s=45); the method returns 45 + 17 = 62.
module Conf;
public class C
  public method Go(): int
    var k := 2; var s := 0;
    for var i in 0..<5 do
      k := k + i;
      var h := lambda() => do k := k + 1; return k; end;
      s := s + h();
    end;
    return s + k;
  end Go;
  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.Go());
    return 0;
  end Main;
end C;
