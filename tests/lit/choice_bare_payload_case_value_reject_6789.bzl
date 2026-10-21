// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #6789 -- spec §35.3, v0.340: "Construction is qualified ...:
// `FetchOutcome.NotModified` for a payload-free case (a value expression, not a
// call), `FetchOutcome.Failed(504, "upstream timeout")` for a payload case."
// A payload case written bare in value position denotes no value. Before the
// fix it compiled under warning #2434 to a zero-initialized value (tag 0).

module M;
choice Outcome NotModified, Fetched(Status: int), Failed(Msg: string); end Outcome;
class Program
  public static method Main()
    var f := Outcome.Fetched;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: payload case `Fetched` cannot be used as a value without its payload: §35.3 constructs a payload case with the call form
    Console.WriteLine("v");
  end Main;
end Program;
