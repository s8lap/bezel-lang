//! case: s28_14_9985
//! doc: spec
//! section: 28.14
//! title: capture-free lambdas are admitted at every position s28_14_9980 to s28_14_9984 rejects
//! rule: A capture-free function value is admitted at each of those positions
//! cites: 28.14, 28.3
//! expect: accept
//! notes: The admitting twin of s28_14_9980 .. s28_14_9984 (#9310): each rejected shape with the capture
//!   removed from the lambda.
module Conf;
public class FBox
  public f: ((double) -> ())? := null;
  public constructor() end constructor;
end FBox;
public record FRec(F: (double) -> ());
public class Hub
  private owned readonly _keep: FBox := new FBox();
  public method On(scoped handler: (double) -> ())
    var r := new FRec(lambda(t: double) => Console.Write(""));
    var arr: Array<(double) -> ()> := [lambda(t: double) => Console.Write("")];
    var g: ((double) -> ())? := null;
    _keep.f := g ?? lambda(t: double) => Console.Write("");
    var k := 0;
    _keep.f := (case k of 0 => lambda(t: double) => Console.Write(""), _ => lambda(t: double) => Console.Write(""));
    var mk := lambda() => lambda(t: double) => Console.Write("");
    _keep.f := mk();
  end On;
end Hub;
