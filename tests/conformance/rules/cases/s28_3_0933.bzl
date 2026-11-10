//! case: s28_3_0933
//! doc: spec
//! section: 28.3
//! title: a ?? over a nullable and a non-nullable named owned Task local is accepted as a borrowed meet
//! rule: borrowed ⊓ borrowed = **borrowed** with the union of sources
//! cites: 30.3
//! expect: accept
module Conf;
public class Program
  static method Go()
    using var t: Task? := Task.Delay(0);
    using var t2 := Task.Delay(0);
    var u := t ?? t2;
  end Go;
end Program;
