# Hydra

Hydra is a server framework for the Windows API based on IO completion ports.
The server class manages the connection lifecycle and events on the completion port
using callbacks implemented as members of an application class. Several parameters
related to network setup and resource allocation are configurable on construction of the server.

#### Changes
- Spent some time building a minimally tested (and totally sketchy) family of allocators.
	Inspired and guided by Bonwick's papers on the slab allocator.
- Better worker synchronization with synchronization barriers.
	Allows more controlled shutdown. Eliminates burden on workers
	to be aware of concurrent shutdown and unnecessary logic in event loop.
- Multi-phase shutdown with timeout for graceful connection termination.
	Timeout is now per-worker and is based on the time from the last arrived packet.
	It is based on a call to GetQueuedCompletionStatus() with a timeout set, so each
	time a new completion packet is dequeued, the timeout resets.

#### Future Revisions
- Class "TCP". Unified interface for client and server functionality, with distinct
	client read/write and server read/write ops and other internal ops to better utilize
	completion queue. Applications will inherit from the server instead of the previous
	template and delegate architecture.
- Unified interface with distinct client read/write and server read/write ops, and other
	internal ops to better utilize completion queue.
- ~~Applications should delete packets on CLOSE error. Server should delete packets on ACCEPT error.
	Packets currently not cleaned up in server, so will leak.~~ Packets will be under the exclusive
	control of applications. Need to figure out how to make this work with AcceptEx, probably will
	delegate responsibility to application.
- Better error interface? Use struct and record source of internal errors.
- Per-connection timeouts?
- UDP
- Thread load balancing? Use timing based QPC to determine when to add or remove threads.
	Mutex contention &mdash; maybe associate new connections with mutexes
	in a manner that reduces contention, some connections may be more frequent and demanding.