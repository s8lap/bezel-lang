module Lib;

public extension Random
  public method Frob(): int => this.Next(1) + 7;
end;
