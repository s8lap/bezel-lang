//! case: s3_6_0052
//! doc: spec
//! section: 3.6
//! title: a generic instance method reads its constraint's Message and an owned field's Message in one result
//! rule: "A generic body type-checks under the weakest assumption its constraints establish" ([§3.6]). The body of `Read<T>` with `where T: Exception` reads `x.Message` through that constraint, and `this.item.Message` reads the same member through the owned field's class type, so the result is the two messages joined.
//! cites: 31.4
//! expect: run
//! output: argheld
module Conf;
public class Holder
  private owned item: Exception;
  public constructor(owned e: Exception)
    this.item := e;
  end constructor;
  public method Read<T>(x: T): string where T: Exception
    return x.Message + this.item.Message;
  end Read;
end Holder;
public class Program
  public static method Main()
    using var h := new Holder(new Exception("held"));
    using var io := new Exception("arg");
    Console.WriteLine(h.Read(io));
  end Main;
end Program;
