public class Handler
  public method Process(n: sbyte): string => "sbyte";
  public method Process(n: ushort): string => "ushort";
end Handler;

var h := new Handler();
var val: byte := 50;

h.Process(val);
// byte→sbyte is NOT implicit (§5.1: signed/unsigned same-or-narrower width never implicit).
//   ⇒ Process(sbyte) is not applicable.
// byte→ushort IS implicit (§5.1, byte row).
//   ⇒ Process(ushort) is the only applicable candidate.
// Resolves to Process(ushort). No ambiguity — only one candidate survived applicability.
