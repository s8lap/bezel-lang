public record ClickedEvent(Sender: Button, X: int, Y: int);

public class Button
  private owned readonly _clicked: EventSource<ClickedEvent> := new();
  public property Clicked: IEvent<ClickedEvent> => _clicked;

  private method OnPress(x: int, y: int)
    _clicked.Raise(new ClickedEvent(this, x, y));
  end OnPress;
end Button;

// Subscription sites:
using var sub := button.Clicked.Subscribe(lambda(e) => UpdateStatus(e.X, e.Y));
