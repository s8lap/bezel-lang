private _onClicked: (ClickedEvent) -> () := lambda(e) => UpdateStatus(e.X, e.Y);
// ...
button.Clicked += _onClicked;
// later:
button.Clicked -= _onClicked;          // same reference; matches and unsubscribes
