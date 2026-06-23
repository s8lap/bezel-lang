// Shared illustrative stubs for conformance example fragments.
//
// Many spec example fragments reference illustrative helper TYPES that the
// surrounding prose assumes exist (Connection, Point, BankAccount, …) without
// defining them in the fragment itself. run_conformance.sh prepends this block
// (its declarations, after the synthesized `module Conf;`) to any case whose
// manifest `notes` field is `preamble`, so those fragments compile against a
// common minimal stub set instead of failing on "unresolved type name".
//
// Stubs are intentionally minimal-but-valid: a class that may be `using`-bound
// gets a constructor + destructor; types carry the members the examples access
// where those are guessable. A fragment that DEFINES one of these names will
// collide ("already declared") and is handled by the harness/manifest (it is
// not tagged `preamble`, or it falls back to skip).

public record Point(X: int, Y: int);
public record Pixel(R: byte, G: byte, B: byte);
public record User(Name: string, Id: int);
public record Customer(Name: string, Id: int);
public record Vertex(X: int, Y: int);

public interface IBankAccount
  method Deposit(amount: decimal);
  method Withdraw(amount: decimal);
  property Balance: decimal;
  readonly property AccountNumber: string;
end IBankAccount;

public class Connection
  public constructor(name: string := "") end constructor;
  public method View(): int => 0;
  public method Close() end Close;
  public method Flush() end Flush;
  destructor end destructor;
end Connection;

public class Cache
  public constructor() end constructor;
  destructor end destructor;
end Cache;

public class Logfile
  public constructor(path: string := "") end constructor;
  destructor end destructor;
end Logfile;

public class TransactionCache
  public constructor() end constructor;
  destructor end destructor;
end TransactionCache;

public class Transaction
  public constructor() end constructor;
  destructor end destructor;
end Transaction;

public class Subscription
  public constructor() end constructor;
  destructor end destructor;
end Subscription;

public class Parser
  public constructor() end constructor;
  destructor end destructor;
end Parser;

public class Item
  public constructor() end constructor;
  destructor end destructor;
end Item;

public class Company
  public constructor() end constructor;
  destructor end destructor;
end Company;

public class Config
  public constructor() end constructor;
  destructor end destructor;
end Config;

public class MyClass
  public constructor() end constructor;
  destructor end destructor;
end MyClass;

public class WaferTest
  public constructor() end constructor;
  destructor end destructor;
end WaferTest;

public class Risky
  public constructor() end constructor;
  destructor end destructor;
end Risky;

public class Animal
  public constructor() end constructor;
  public virtual method Speak(): string => "...";
  destructor end destructor;
end Animal;

public class Dog: Animal
  public constructor() end constructor;
  public override method Speak(): string => "Woof!";
  public method Bark() end Bark;
end Dog;

public class Shape
  public constructor() end constructor;
  public virtual method Area(): double => 0.0;
  destructor end destructor;
end Shape;

public class Circle: Shape
  public constructor() end constructor;
  public override method Area(): double => 3.14;
end Circle;

public class Square: Shape
  public constructor() end constructor;
  public override method Area(): double => 1.0;
end Square;

public class Rectangle: Shape
  public constructor() end constructor;
  public override method Area(): double => 1.0;
end Rectangle;

public enum Priority Low, Medium, High; end Priority;
public enum MouseButton Left, Right, Middle; end MouseButton;
