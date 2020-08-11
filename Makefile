CPPFLAGS=$(CPPFLAGS) /EHsc /MDd

httphello.exe : httphello.obj Connection.obj util.obj Logger.obj
	LINK /DEBUG httphello.obj Connection.obj util.obj Logger.obj Ws2_32.lib
	
clean :
	del *.obj *.exe *.ilk *.pdb