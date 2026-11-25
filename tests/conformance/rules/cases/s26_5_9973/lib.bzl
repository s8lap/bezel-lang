module Lib;

public extension Exception
  public method Tag(): string => "tag:" + this.Message;
end;
