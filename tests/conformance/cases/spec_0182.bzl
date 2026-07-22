button.Clicked += lambda(e) => UpdateStatus(e.X, e.Y);
// later:
button.Clicked -= lambda(e) => UpdateStatus(e.X, e.Y);   // a different lambda, no match
