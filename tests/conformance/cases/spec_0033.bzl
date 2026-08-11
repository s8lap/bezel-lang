// A parser validating a wire-read value (wire, trusted: int; ProtocolException
// stands for a user exception class):
var p := Priority.FromUnderlyingOrNull(wire)
         ?? throw new ProtocolException($"bad priority {wire}");    // reject
var p2 := Priority.FromUnderlyingOrNull(wire) ?? Priority.Low;      // tolerate with a default
var p3 := Priority(trusted);   // the checked cast is the assert: failure is a bug (§5.4)
