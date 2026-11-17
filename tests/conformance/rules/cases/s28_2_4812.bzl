//! case: s28_2_4812
//! doc: spec
//! section: 28.2
//! title: a use of an alias after the owner was disposed is rejected
//! rule: The same error reaches a use through a local borrow of the
//!   variable's value
//! cites: 28.2, 28.3.1
//! expect: reject
//! diagnostic: local borrow of that value
module Conf;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;
  public static async method Main(): Task
    var t := Program.W();
    var b := t;
    t.Dispose();
    await b;
  end Main;
end Program;
