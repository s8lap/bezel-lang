//! case: s23_3_8102
//! doc: spec
//! section: 23.3
//! title: caller locals live across an inlined setter keep their values despite same-named
//!  setter locals
//! rule: The setter's implicit `value` parameter has no modifier by default and takes a
//!  borrow of the property's declared type.
//! cites: 23.3, 28.9
//! expect: run
//! output: 5
//!   8
//!   OUT
//! notes: Pins #9019 (narrow). The setter's locals s and t share names with the caller's.
//!  The setter sees value "w1b": its s is "w1bq", its t "w1bqz", so it prints 5. The
//!  caller's s is "w1" (2), t is "w1b" (3) and outer is "OUT" (3), so it prints 8, then
//!  OUT.
module Conf;
public class H
  public property P: string
    get => "x";
    set => do
      var s := value + "q";
      var t := s + "z";
      Console.WriteLine(t.Length);
    end;
  end;
end H;
public class Program
  public static method Main(): int
    using var h := new H();
    var outer := "OUT";
    var s := "w" + "1";
    var t := s + "b";
    h.P := t;
    Console.WriteLine(s.Length + t.Length + outer.Length);
    Console.WriteLine(outer);
    return 0;
  end Main;
end Program;
