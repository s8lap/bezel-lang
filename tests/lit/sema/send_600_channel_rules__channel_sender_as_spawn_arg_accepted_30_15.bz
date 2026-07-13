// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, channel_sender_as_spawn_arg_accepted_30_15).
//
// §30.15 stdlib §6.9: ChannelSender<T> is sendable and may be passed as a
// spawn argument. The canonical multi-producer pattern.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; sealed class Report public Id: int := 0; destructor end destructor; end Report; class Worker public static method ProcessAll(owned tx: ChannelSender<Report>) using var sender := tx; sender.Send(new Report()); end ProcessAll; end Worker; class Main public static async method Go(): Task using var inbox := new Channel<Report>(); using var t := Domain.Spawn(Worker.ProcessAll, inbox.NewSender()); await t; end Go; end Main;

// NODIAG-NOT: error
// NODIAG-NOT: §
