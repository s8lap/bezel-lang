public class Calculator
  public method Compute(a: int, b: long): string => "int,long";
  public method Compute(a: long, b: int): string => "long,int";
end Calculator;

var calc := new Calculator();
var x: int := 5;
var y: int := 10;

calc.Compute(x, y);
// arg a (int): exact in Compute(int,long); int→long conversion in Compute(long,int).
// arg b (int): int→long conversion in Compute(int,long); exact in Compute(long,int).
// Each candidate is strictly better on a different argument; neither dominates.
// Ambiguous call — compile error citing both candidates.
// Disambiguate:
//   calc.Compute(x, long(y));    // makes Compute(int,long) exact on both ⇒ selects it
//   calc.Compute(long(x), y);    // makes Compute(long,int) exact on both ⇒ selects it
