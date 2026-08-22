// Visibility? Modifier* KIND Name GenericParams? (primaryParams)? Inheritance? GenericConstraints* body
public sealed class BankAccount(name: string, start: decimal): Account(name), IAudited
end BankAccount;                          // or `end;`
