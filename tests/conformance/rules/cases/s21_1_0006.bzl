//! case: s21_1_0006
//! doc: spec
//! section: 21.1
//! title: a runtime const initialized from a borrow-returning method is admitted and reads the borrowed array
//! rule: and **borrows** — the binding is then a borrow carrying the initializer's source-liveness constraints
//! cites: 21.1
//! expect: run
//! output: 4
module Conf;

class Holder
  private owned _bytes: Array<byte>;
  public constructor() _bytes := new Array<byte>(4, 7); end constructor;
  public method ToByteArray(): Array<byte> => _bytes;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    const b := h.ToByteArray();
    Console.WriteLine(b.Count);
  end Main;
end Program;
