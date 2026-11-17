//! case: s28_3_1_0005
//! doc: spec
//! section: 28.3.1
//! title: an outer task disposed in a loop body is maybe-consumed at the back
//!   edge and rejected
//! rule: owned ⊓ explicitly-disposed = **maybe-consumed** — unreadable after
//!   the join, no flow around a back edge into a use
//! cites: 28.3.1, 28.2
//! expect: reject
//! diagnostic: may have already been disposed
module Conf;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;
  public static async method Main(): Task
    var t := Program.W();
    await t;
    var j: int := 0;
    while j < 3 do
      t.Dispose();
      j := j + 1;
    end while;
  end Main;
end Program;
