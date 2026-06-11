button.Clicked += (e) => UpdateStatus(e.X, e.Y);
// later:
button.Clicked -= (e) => UpdateStatus(e.X, e.Y);   // a different lambda, no match
