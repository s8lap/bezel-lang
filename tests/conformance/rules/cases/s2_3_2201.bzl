//! case: s2_3_2201
//! doc: spec
//! section: 2.3
//! title: a user class named Stack shadows the always-available Stack and
//!   its own TryPeek is the one called
//! rule: every other always-available name may be shadowed by local
//!   declarations under the normal name-resolution rules
//! cites: 22.3
//! expect: run
//! stdout: 300
//! notes: Trace: the local declaration shadows the library Stack (spec
//!   2.3), so new Stack<string>() builds the user class; its Push
//!   stores the string in _item and its TryPeek assigns the out and
//!   returns true, so each of the 100 iterations adds the 3-character
//!   string: 300. Green on the pre-fix compiler and the tip: a control
//!   against a by-name Try special case.
module Conf;

class Stack<T>
  private _item: T? := null;
  public method Push(x: T)
    _item := x;
  end Push;
  public method TryPeek(out item: T?): bool
    item := _item;
    return true;
  end TryPeek;
end Stack;

public class Program
  public static method Main()
    var tot := 0;
    var i := 0;
    while i < 100 do
      var s := new Stack<string>();
      s.Push("a" + (40 + i mod 10).ToString());
      var v: string? := null;
      if s.TryPeek(out v) then tot := tot + (v ?? "").Length; end;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
