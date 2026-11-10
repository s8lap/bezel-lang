//! case: s28_3_0930
//! doc: spec
//! section: 28.3
//! title: a ?? mixing a borrowed Task parameter with a fresh Task is rejected
//! rule: mixed = **rejected** — equalize the arms first
//! cites: 30.3, 22.3
//! expect: reject
//! diagnostic: mixed
//! issue: 7807
module Conf;
public class Program
  static method Go(t: Task?)
    var u := t ?? Task.Delay(0);
  end Go;

  public static method Main()
    Console.WriteLine(1);
  end Main;
end Program;
