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

  // Expression-position case arms cannot carry statements (§19.2), so the transition
  // function is a statement case that returns from each arm.
  private method Next(s: State, e: Event): State
    case s of
      Idle => case e of
          Coin(var n) => do return State.HasCoin(n); end,
          Select(_) => do Console.WriteLine("insert coin first"); return s; end,
          Cancel => do Console.WriteLine("nothing to refund"); return s; end
        end case,
      HasCoin(var a) => case e of
          Coin(var n) => do return State.HasCoin(a + n); end,
          Select(var p) when a >= p => do return State.Vending(a - p); end,
          Select(var p) => do Console.WriteLine($"insufficient: {a} < {p}"); return s; end,
          Cancel => do Console.WriteLine($"refund {a}"); return State.Idle; end
        end case,
      Vending(_) => do return State.Idle; end
    end case;
  end Next;

  public method Handle(e: Event)
    var before := _state;
    _state := Next(_state, e);
    Console.WriteLine($"{before} -> {_state}");
    case _state of
      Vending(var c) => do
        Console.WriteLine($"vend, change {c}");
        _state := State.Idle;
      end,
      _ => ()
    end case;
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
