public class Converter
  public method Convert(n: long): string => "long";
  public method Convert(n: float): string => "float";
end Converter;

var cvt := new Converter();
var a: int := 10;

cvt.Convert(a);
// a is int. Neither candidate is an exact match.
//   Convert(long):  int→long  is implicit (§5.1).
//   Convert(float): int→float is implicit (§5.1).
// Both conversions are admitted by §5.1 and are equally ranked: neither
// candidate is better at the single argument position.
// Ambiguous call — compile error citing both candidates.
// Disambiguate with an explicit cast so exactly one is exact:
//   cvt.Convert(long(a));     // selects Convert(long)
//   cvt.Convert(float(a));    // selects Convert(float)
