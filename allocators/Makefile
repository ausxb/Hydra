CPPFLAGS=$(CPPFLAGS) /EHsc /MDd /Zi

httphello.exe : httphello.obj Connection.obj util.obj Logger.obj
	LINK /DEBUG httphello.obj Connection.obj util.obj Logger.obj Ws2_32.lib

alloctest.exe : alloctest.obj RawSlabCache.obj EmbeddedSlabCache.obj ObjectAllocator.obj ../util.obj
	LINK /DEBUG alloctest.obj RawSlabCache.obj EmbeddedSlabCache.obj ObjectAllocator.obj util.obj Ws2_32.lib

allocprof.exe : allocprof.obj RawSlabCache.obj EmbeddedSlabCache.obj ObjectAllocator.obj ../util.obj
	LINK /DEBUG allocprof.obj RawSlabCache.obj EmbeddedSlabCache.obj ObjectAllocator.obj util.obj Ws2_32.lib

clean :
	del *.obj *.exe *.ilk *.pdb
