//! case: s26_1_0008
//! doc: spec
//! section: 26.1
//! title: `this` in an extension over string reaches bool.TryParse as the receiver string
//! rule: `this` refers to the receiver.
//! cites: 26.1, 9.4
//! expect: run
//! issue: 7304
//! output: 1
//!   0

module Conf;

extension string
  public method IsTrue(): int
    var b: bool := false;
    var ok: bool := bool.TryParse(this, out b);
    if ok and b then
      return 1;
    end;
    return 0;
  end IsTrue;
end;

public class Program
  public static method Main()
    Console.WriteLine("true".IsTrue());
    Console.WriteLine("nope".IsTrue());
  end Main;
end Program;
