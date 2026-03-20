CLANGPP := ~/code/llvm-project/build/bin/clang++
OPTLLVM := ~/code/llvm-project/build/bin/opt
INCLUDE := -I./parlaylib/include/ -I/usr/include/libunwind/
DBGFLAG := -ggdb
LIBRARY := -lunwind -lpthread
OPTIONS := -xc++ -stdlib=libc++ -std=c++17

scheduler: scheduler.cpp
	$(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) $(DBGFLAG) $< -o $@

scheduler.i: scheduler.cpp
	$(CLANGPP) $(INCLUDE) $(OPTIONS) $(DBGFLAG) -E $< -o $@

scheduler.s: scheduler.cpp
	$(CLANGPP) $(INCLUDE) $(OPTIONS) $(DBGFLAG) -S $< -o $@

scheduler.o: scheduler.cpp
	$(CLANGPP) $(INCLUDE) $(OPTIONS) -S $< -o $@

scheduler.ll: scheduler.cpp
	$(CLANGPP) $(INCLUDE) $(OPTIONS) -S -emit-llvm $< -o $@

scheduler.opt.ll: scheduler.ll
	$(OPTLLVM) -S -passes=mem2reg $< -o $@

.PHONY: clean
clean:
	rm scheduler scheduler.ll scheduler.opt.ll scheduler.i
