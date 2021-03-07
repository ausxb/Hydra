# Hydra

Hydra is a networking framework for the Windows API based on IO completion ports.
The protocol class manages the connection lifecycle and events on the completion port
using callbacks implemented as members of an application class.

The example may be built using the VS command prompt by running `nmake httphello.exe`
from the source root. It illustrates one application listening on both port 80 and 8080.

#### Changes
- Replaced class `TCPServer` with `TCP`, creating a unified interface for client and server
	functionality. Applications inherit from `TCP` and implement callbacks for network
	events.
- New design employs distinct client and server read/write and connection ops along with
	internal messages to fully utilize the completion queue.
- Multiple server ports can be operated on the same completion port. Each port is opened
	independently via a call to `TCP::listen()` which returns a reference to a `TCP::OpenPort`.
	That reference is a stateful handle, but is opaque from the application's perspective.
- Concurrent shutdown is now supported. One port can be shutdown, while others are still running,
	by passing its reference to `TCP::close()`.


- Spent some time building a minimally tested (and totally sketchy) family of allocators.
	Inspired and guided by Bonwick's papers on the slab allocator.

#### Future Revisions
- Callbacks.dox needs to be updated or removed
- Client operations: `TransmitFile(TF_DISCONNECT)` and `ConnectEx()`
- Per-connection timeouts?
- UDP
- Timing facilities with `QueryPerformanceCounter()`
