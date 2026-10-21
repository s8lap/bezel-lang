//! case: s17_5_0008
//! doc: spec
//! section: 17.5
//! title: an int left operand widens to long for membership in an Array<long>
//! rule: "`x in c` *is* the call `c.Contains(x)`, so the left operand is
//!   admitted exactly when it is admissible as that call's argument under the
//!   ordinary conversion rules ([§5.1])." The §5.1 table lists `int` as
//!   implicitly converting to `long`.
//! cites: 17.5, 5.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a: Array<long> := [1, 2];
    var i: int := 2;
    var found := i in a;
    Console.WriteLine(found);
  end Main;
end Program;
