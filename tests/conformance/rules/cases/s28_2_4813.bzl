//! case: s28_2_4813
//! doc: spec
//! section: 28.2
//! title: a task disposed on one branch is maybe-consumed and a later await
//!   is rejected
//! rule: at a join of an explicitly-disposed path with a still-owned path the
//!   variable is **maybe-consumed** and a later use is a compile error
//! cites: 28.2, 28.3.1
//! expect: reject
//! diagnostic: may have already been disposed
module Conf;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;
  public static async method Main(): Task
    var t := Program.W();
    var i: int := 0;
    if i mod 2 = 0 then
      t.Dispose();
    end if;
    await t;
  end Main;
end Program;
