module StateMachine.Vending;

public choice State
  Idle,
  HasCoin(amount: int),
  Vending(change: int);

  public override method ToString(): string => (case this of
    Idle => "Idle",
    HasCoin(var a) => $"HasCoin({a})",
    Vending(var c) => $"Vending({c})");
end State;

public choice Event
  Coin(n: int),
  Select(price: int),
  Cancel;
end Event;

public class Machine
  private _state: State := State.Idle;

  public method Handle(e: Event)
    var before := _state;
    _state := (case _state of
      Idle => (case e of
        Coin(var n) => State.HasCoin(n),
        Select(_) => do Console.WriteLine("insert coin first"); return State.Idle; end,
        Cancel => do Console.WriteLine("nothing to refund"); return State.Idle; end),
      HasCoin(var a) => (case e of
        Coin(var n) => State.HasCoin(a + n),
        Select(var p) when a >= p => State.Vending(a - p),
        Select(var p) => do Console.WriteLine($"insufficient: {a} < {p}"); return _state; end,
        Cancel => do Console.WriteLine($"refund {a}"); return State.Idle; end),
      Vending(_) => State.Idle);
    Console.WriteLine($"{before} -> {_state}");
    if _state is Vending then
      case _state of
        Vending(var c) => Console.WriteLine($"vend, change {c}"),
        _ => ()
      end;
      _state := State.Idle;
    end;
  end Handle;
end Machine;

public class Program
  public static method Main()
    using var m := new Machine();
    var events: Array<Event> := [Event.Coin(50), Event.Select(75), Event.Coin(50), Event.Select(75), Event.Cancel];
    for var e in events do
      m.Handle(e);
    end;
  end Main;
end Program;
