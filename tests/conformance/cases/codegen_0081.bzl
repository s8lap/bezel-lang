public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0 and value <= 65535)

    Value := value;
  end constructor;
end Port;

var p := new Port(8080);           // a constructor or factory call
var opt: Port? := null;            // where a zero value is genuinely wanted: Port? / default(Port?)
public method WithValue(v: int): Port => new Port(v);      // the sanctioned update shape
