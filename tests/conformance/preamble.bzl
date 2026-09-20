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
  private _name: string := "";
  public constructor(name: string := "")
    _name := name;
  end constructor;
  // 2026-09-21: the members the §28/§29.12 owning-collection fragments and
  // Guide 5 §1/§3/§6 read off a pooled connection. Each is forced by its call
  // site — `conn.Ping()`, `c.SendShutdown()`, `b.Configure()` are bare
  // statements (no arguments, result discarded); `c.Name` is interpolated /
  // written to Console.WriteLine (a string); `c.IsBusy()` is a lambda body
  // under `not` (a bool).
  public property Name: string => _name;
  public method View(): int => 0;
  public method Close() end Close;
  public method Flush() end Flush;
  public method Ping() end Ping;
  public method Configure() end Configure;
  public method SendShutdown() end SendShutdown;
  public method IsBusy(): bool => false;
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
  // spec §29.12 Mutation during iteration reads `if t.IsExpired then …` — a
  // bool, forced by its position as the `if` condition.
  public property IsExpired: bool => false;
  destructor end destructor;
end Transaction;

public class Subscription
  public constructor() end constructor;
  destructor end destructor;
end Subscription;

public class Parser
  public constructor() end constructor;
  // v0.247 regeneration: the Guide 1/2 `Import` examples construct a Parser
  // over an open FileStream and drain it (`parser.ReadAll()`).
  public constructor(fs: FileStream) end constructor;
  public method ReadAll() end ReadAll;
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
