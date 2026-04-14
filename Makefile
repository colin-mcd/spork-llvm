CLANGPP := ~/code/llvm-project/build/bin/clang++ -stdlib=libc++
# CLANGPP := g++
OPTLLVM := ~/code/llvm-project/build/bin/opt
#INCLUDE := -I./parlaylib/include/ -I/usr/include/libunwind/
INCLUDE := -I./parlaylib/include/
DBGFLAG := -ggdb
#LIBRARY := -lpthread -lunwind
LIBRARY := -lpthread
SETPATH := LD_PRELOAD=/usr/local/lib/libjemalloc.so
OPTIONS := -xc++ -std=c++20
LLVMOPT := -fpass-plugin=./pass/build/RtsSporkPass.so

scheduler: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) $(DBGFLAG) $< -o $@

schedulerO1: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) $(DBGFLAG) -O1 $< -o $@
schedulerO2: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) $(DBGFLAG) -O2 $< -o $@
schedulerO3: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) $(DBGFLAG) -O3 $< -o $@

schedulerO1.s: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) -O1 -S $< -o $@
schedulerO2.s: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) -O2 -S $< -o $@
schedulerO3.s: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) -O3 -S $< -o $@

scheduler.i: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(OPTIONS) $(DBGFLAG) -E $< -o $@

scheduler.s: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(OPTIONS) $(DBGFLAG) -S $< -o $@

scheduler.o: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(OPTIONS) -S $< -o $@

scheduler.ll: scheduler.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(OPTIONS) -O2 -S -emit-llvm $< -o $@

scheduler.opt.ll: scheduler.ll *.hpp Makefile
	$(SETPATH) $(OPTLLVM) -S -O3 $< -o $@

pass/build/RtsSporkPass.so: pass/RtsSporkPass.cpp pass/rts_spork_table.h
	$(MAKE) -C pass/build

llvmtest: scheduler.cpp *.hpp pass/build/RtsSporkPass.so
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(OPTIONS) $(LIBRARY) $(DBGFLAG) $(LLVMOPT) $< -o $@

.PHONY: clean
clean:
	rm scheduler schedulerO1 schedulerO2 schedulerO3 scheduler.ll scheduler.opt.ll scheduler.i schedulerO1.s schedulerO2.s schedulerO3.s
