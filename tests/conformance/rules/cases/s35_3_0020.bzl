//! case: s35_3_0020
//! doc: spec
//! section: 35.3
//! title: a block-bodied property getter return is a declared-result position
//! rule: "the top-level operand of a `return e;` statement in a body with a declared return type (§21.7); the expression body of a member or accessor checked against a declared type (§22.2, §23.2, §23.3)" (§35.3, v0.340). The block getter of `F: Fetch` returns bare `NotModified` and `Got("blk")`.
//! cites: 23.3, 21.7
//! expect: run
//! output: blk
//!   nm
module Conf;

public choice Fetch
  Got(body: string),
  NotModified;
end Fetch;

public class Box
  public flag: bool;

  public property F: Fetch
    get => do
      if flag then return NotModified; end;
      return Got("blk");
    end;
  end F;
end Box;

public class Program
  public static method Main()
    var b := new Box();
    Console.WriteLine((case b.F of Got(var s) => s, NotModified => "nm"));
    b.flag := true;
    Console.WriteLine((case b.F of Got(var s) => s, NotModified => "nm"));
  end Main;
end Program;
