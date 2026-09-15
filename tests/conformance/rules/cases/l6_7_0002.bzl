//! case: l6_7_0002
//! doc: stdlib
//! section: 6.7
//! title: discarding a Task with `_ := ...;` is rejected — FireAndForget is required
//! rule: FireAndForget is the only supported way to discard a Task. The
//!   discard form `_ := someAsyncCall();` is rejected at compile time.
//! cites: 6.7
//! expect: reject
//! diagnostic: FireAndForget
module Conf;

public class Program
  public static async method Load(): Task
    await Task.Delay(0);
  end Load;

  public static method Main()
    _ := Load();
  end Main;
end Program;
