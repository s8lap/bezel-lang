interface INumber<T> where T: INumber<T>
  static method Zero(): owned T;
  static operator +(a: T, b: T): owned T;
  static operator *(a: T, b: T): owned T;
end INumber;
