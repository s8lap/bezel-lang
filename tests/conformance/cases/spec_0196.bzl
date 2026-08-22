// The lambda form with deterministic teardown — and the form that also works
// anywhere, since Subscribe is on the IEvent<T> face:
using var sub := button.Clicked.Subscribe(lambda(e) => UpdateStatus(e.X, e.Y));
// disposing sub severs the registration (stdlib §6.5)
