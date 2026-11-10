//! case: s28_10_4201
//! doc: spec
//! section: 28.10
//! title: a using var pin over an owning inline-array field freezes the holder against a move
//! rule: The `using var` still **pins** the borrow's source for the duration of the scope
//! cites: 28.10, 28.3, 15.1.1
//! expect: reject
//! diagnostic: pins
//! issue: 8454
module Conf;

public class Res
  public constructor() end constructor;
end Res;

public class H
  public Arr: [2 owned]Res? := [new Res(), new Res()];
  public constructor() end constructor;
end H;

public class Program
  public static method Main()
    var h := new H();
    using var x := h.Arr;
    using var h2 := move h;
  end Main;
end Program;
