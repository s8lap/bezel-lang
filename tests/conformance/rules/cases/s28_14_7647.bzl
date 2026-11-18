//! case: s28_14_7647
//! doc: spec
//! section: 28.14
//! title: an async method may not store its call-bounded function parameter in a field
//! rule: It may not be stored in a field of any object
//! cites: 28.14, 30.1
//! expect: reject
//! diagnostic: call-bounded
module Conf;
import System;
class Holder
  public F: ((int) -> int)? := null;
  public async method Keep(h: (int) -> int): Task<int>
    await Task.Delay(1);
    F := h;
    return 1;
  end Keep;
end Holder;
class C
  public static async method Main(): Task<int>
    using var o := new Holder();
    var a := await o.Keep(lambda(v: int): int => v + 1);
    Console.WriteLine(a);
    return 0;
  end Main;
end C;
