//! case: s28_3_0931
//! doc: spec
//! section: 28.3
//! title: a ?? mixing a named owned OwningTask local with a fresh OwningTask is rejected
//! rule: mixed = **rejected** — equalize the arms first
//! cites: 30.3, 20
//! expect: reject
//! diagnostic: mixed
//! issue: 7807
module Conf;
public class R
  public constructor() end constructor;
end R;
public class Program
  static async method P(): OwningTask<R>
    return new R();
  end P;

  static method Go()
    using var t: OwningTask<R>? := P();
    var u := t ?? P();
  end Go;

  public static method Main()
    Console.WriteLine(1);
  end Main;
end Program;
