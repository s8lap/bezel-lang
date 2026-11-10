//! case: s28_3_0934
//! doc: spec
//! section: 28.3
//! title: a ?? over a borrowed Task? parameter and a borrowed Task parameter is accepted as a borrowed meet
//! rule: borrowed ⊓ borrowed = **borrowed** with the union of sources
//! cites: 30.3, 22.3
//! expect: accept
module Conf;
public class Program
  static method Go(t: Task?, b: Task)
    var u := t ?? b;
  end Go;
end Program;
