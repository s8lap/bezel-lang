//! case: s9_3_0011
//! doc: spec
//! section: 9.3
//! title: a body record's non-defaultable field is satisfied by its constructor's assignment
//! rule: assigned a value in every constructor before the constructor returns
//! cites: 9.2, 9.3, 7.9
//! expect: run
//! issue: 8097
//! stdout: q
module Conf;

public record Holder
  private _c: string;

  public constructor(c: string)
    _c := c;
  end constructor;

  public method C(): string => _c;
end Holder;

public class Program
  public static method Main()
    var h := new Holder("q");
    Console.WriteLine(h.C());
  end Main;
end Program;
