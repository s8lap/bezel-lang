//! case: s35_3_0030
//! doc: spec
//! section: 35.3
//! title: a bare case construction in an unascribed lambda block is not target-typed
//! rule: "not to lambdas whose return type arrives only by target inference (no ascription written)" (§35.3, v0.340): the block lambda has no ascription, so `Got("inner")` names nothing.
//! cites: 35.5
//! expect: reject
//! diagnostic: Got
module Conf;

public choice Fetch
  Got(body: string),
  NotModified;
end Fetch;

public class Program
  public static method P(): Fetch
    var f := lambda() => do
      return Got("inner");
    end;
    return NotModified;
  end P;

  public static method Main() end Main;
end Program;
