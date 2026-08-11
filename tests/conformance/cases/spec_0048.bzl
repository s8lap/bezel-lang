interface INumber<T> where T: INumber<T>
  static method Zero(): owned T;
  static operator +(a: T, b: T): owned T;
  static operator *(a: T, b: T): owned T;
end INumber;

public static method Sum<T>(items: Array<T>): owned T where T: INumber<T>
  var total := T.Zero();                          // owned at class/interface T; a plain value otherwise
  for var x in items do total := total + x; end;  // fresh result in; displaced total disposed (§28.7)
  return total;                                   // transfers; a by-value return at value T
end Sum;
