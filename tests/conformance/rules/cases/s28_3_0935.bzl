//! case: s28_3_0935
//! doc: spec
//! section: 28.3
//! title: an if-expression over two named owned Task locals is accepted as a borrowed meet
//! rule: The **meet** of the arms' ownership states, identically to the `a ?? b` row above
//! cites: 30.3, 20
//! expect: accept
module Conf;
public class Program
  static method Go(flag: bool)
    using var t: Task? := Task.Delay(0);
    using var t2 := Task.Delay(0);
    var u := (if flag then t! else t2);
  end Go;
end Program;
