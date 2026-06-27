// Construction-style conversion: factory method on the destination type.
record Money(Amount: decimal, Currency: string)
  public static method FromDecimal(amount: decimal, currency: string): Money =>
    new Money(amount, currency);
end Money;

// Projection-style conversion: instance method on the source type.
// (Projects onto the preamble `User(Name: string, Id: int)` — the source's
// Email is dropped; field order/types must match the target record, §9.1.)
record UserDto(Id: int, Name: string, Email: string)
  public method ToUser(): User => new User(Name, Id);
end UserDto;
