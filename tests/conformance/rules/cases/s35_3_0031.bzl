//! case: s35_3_0031
//! doc: spec
//! section: 35.3
//! title: a bare case name in a lambda typed only by its target is not target-typed
//! rule: "not to lambdas whose return type arrives only by target inference (no ascription written)" (§35.3, v0.340): the target `() -> Fetch` supplies the type, with no ascription, so `NotModified` names nothing.
//! cites: 35.5
//! expect: reject
//! diagnostic: NotModified
module Conf;

public choice Fetch
  Got(body: string),
  NotModified;
end Fetch;

public class Program
  public static method Main()
    var f: () -> Fetch := lambda() => NotModified;
  end Main;
end Program;
