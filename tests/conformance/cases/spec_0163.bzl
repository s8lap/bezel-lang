// Construction-style conversion: factory method on the destination type.
record Money(Amount: decimal, Currency: string)
  public static method FromDecimal(amount: decimal, currency: string): Money =>
    new Money(amount, currency);
end Money;

// Projection-style conversion: instance method on the source type.
record UserDto(Id: int, Name: string, Email: string)
  public method ToUser(): User => new User(Id, Name, Email);
end UserDto;
