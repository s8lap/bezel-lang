_clicked += lambda(e) => UpdateStatus(e.X, e.Y);
// later:
_clicked -= lambda(e) => UpdateStatus(e.X, e.Y);   // a different lambda, no match
