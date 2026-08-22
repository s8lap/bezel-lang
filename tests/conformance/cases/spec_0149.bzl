public class Transformer  // declaration sketch; method bodies elided
  public method Transform(x: int);
  public method Transform(x: decimal);
end Transformer;

var t := new Transformer();
var n: float := 2.5f;

t.Transform(n);
// float→int is not implicit (§5.1: float's only implicit target is double).
// float→decimal is not implicit (§5.1: decimal and binary floats never mix implicitly).
// No candidate is applicable ⇒ compile error, no candidates cited (none reachable).
// The caller must convert explicitly, e.g. t.Transform(int(n)) or t.Transform(decimal(n)).
