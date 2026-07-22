using var sub := button.Clicked.Subscribe(lambda(e) => UpdateStatus(e.X, e.Y));
