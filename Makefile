SOURCE_DIR=$(MAKEDIR)

!IF !DEFINED(OUTPUT_DIR)
OUTPUT_DIR=build
!ENDIF

!IF !EXIST($(OUTPUT_DIR))
MAKE_OUTPUT_DIR=mkdir $(OUTPUT_DIR)
!ELSE
MAKE_OUTPUT_DIR=rem
!ENDIF

CPPFLAGS=$(CPPFLAGS) /EHsc /MDd /Zi

.cpp.obj:
	cl $(CPPFLAGS) /c /Fo"$(OUTPUT_DIR)/" "$(SOURCE_DIR)\$<"

httphello.exe : output_dir httphello.obj TCP.obj Connection.obj allocators/EmbeddedSlabCache.obj \
	util.obj Logger.obj
	cd $(OUTPUT_DIR)
	link /DEBUG httphello.obj TCP.obj Connection.obj EmbeddedSlabCache.obj util.obj Logger.obj Ws2_32.lib

alloctest.exe : output_dir allocators/alloctest.obj allocators/RawSlabCache.obj allocators/EmbeddedSlabCache.obj \
	allocators/ObjectAllocator.obj util.obj
	cd $(OUTPUT_DIR)
	link /DEBUG alloctest.obj RawSlabCache.obj EmbeddedSlabCache.obj ObjectAllocator.obj util.obj Ws2_32.lib

allocprof.exe : output_dir allocators/allocprof.obj allocators/RawSlabCache.obj allocators/EmbeddedSlabCache.obj \
	allocators/ObjectAllocator.obj util.obj
	cd $(OUTPUT_DIR)
	link /DEBUG allocprof.obj RawSlabCache.obj EmbeddedSlabCache.obj ObjectAllocator.obj util.obj Ws2_32.lib

output_dir :
	$(MAKE_OUTPUT_DIR)

clean :
	del *.obj *.exe *.ilk *.pdb
	del /Q build
	rmdir build
