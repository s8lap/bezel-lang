//! case: s23_3_8101
//! doc: spec
//! section: 23.3
//! title: a setter body that derives string locals from value and passes them to a
//!  borrowing callee
//! rule: The setter's implicit `value` parameter has no modifier by default and takes a
//!  borrow of the property's declared type.
//! cites: 23.3, 28.9
//! expect: run
//! output: 4
//!   3
//! notes: Pins #9019 (narrow). The setter body is inlined at the assignment; value is the
//!  borrowed string "v12", r is "v12q" (4), t is "v12" (3).
module Conf;
public static class Util
  public static method Len(s: string): int return s.Length; end Len;
end Util;
public class H
  public property P: string
    get => "x";
    set => do
      var r := value + "q";
      Console.WriteLine(Util.Len(r));
      var t := value;
      Console.WriteLine(Util.Len(t));
    end;
  end;
end H;
public class Program
  public static method Main(): int
    using var h := new H();
    var i := 12;
    h.P := "v" + i.ToString();
    return 0;
  end Main;
end Program;
