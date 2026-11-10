//! case: s28_3_0929
//! doc: spec
//! section: 28.3
//! title: a ?? mixing a named owned Task<int> local with a fresh Task<int> is rejected
//! rule: mixed = **rejected** — equalize the arms first
//! cites: 30.3, 20
//! expect: reject
//! diagnostic: mixed
//! issue: 7807
module Conf;
public class Program
  static method Go()
    using var t: Task<int>? := Task.FromResult<int>(1);
    var u := t ?? Task.FromResult<int>(2);
  end Go;

  public static method Main()
    Console.WriteLine(1);
  end Main;
end Program;
