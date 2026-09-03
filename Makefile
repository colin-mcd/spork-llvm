# The pass plugins are built against ./llvm-project, so use that clang.
CLANGPP := ./llvm-project/build/bin/clang++ -stdlib=libc++
# CLANGPP := $(HOME)/code/llvm-project/build/bin/clang++ -stdlib=libc++
#CLANGPP := clang++ -stdlib=libc++
# CLANGPP := g++
# OPTLLVM := ~/code/llvm-project/build/bin/opt
OPTLLVM := opt
#INCLUDE := -I./parlaylib/include/ -I/usr/include/libunwind/
INCLUDE := -I./parlaylib/include/
DBGFLAG := -ggdb
#LIBRARY := -lpthread -lunwind
LIBRARY := -lpthread
SETPATH := LD_PRELOAD=/usr/local/lib/libjemalloc.so
OPTIONS := -xc++ -std=c++20 -O3
# Which Spork unroll plugin to use: fablepass (probe-driven) or gempass.
PASS ?= fablepass
LLVMOPT := -fpass-plugin=./$(PASS)/build/SporkUnroll.so

%: %.cpp *.hpp Makefile $(PASS)/build/SporkUnroll.so
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) $(LLVMOPT) $(DBGFLAG) $< -o $@
%.ss: %.cpp *.hpp Makefile $(PASS)/build/SporkUnroll.so
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) $(LLVMOPT) -DUSE_SIGNAL_SAFE_ATOMIC $(DBGFLAG) $< -o $@

# scheduler_expanded.cpp: scheduler.cpp *.hpp Makefile
# 	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) -E $< -o $@

%.s: %.cpp *.hpp Makefile $(PASS)/build/SporkUnroll.so
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) $(LLVMOPT) -S $< -o $@

%.i: %.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(OPTIONS) $(DBGFLAG) -E $< -o $@

%.o: %.cpp *.hpp Makefile
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(OPTIONS) $(LLVMOPT) -S $< -o $@

%.ll: %.cpp *.hpp Makefile $(PASS)/build/SporkUnroll.so
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(OPTIONS) $(LLVMOPT) -O2 -S -emit-llvm $< -o $@

%.opt.ll: %.ll *.hpp Makefile
	$(SETPATH) $(OPTLLVM) -S -O3 $< -o $@

pass/build/RtsSporkPass.so: pass/RtsSporkPass.cpp pass/rts_spork_table.h Makefile
	$(MAKE) -C pass/build

gempass/build/SporkUnroll.so: gempass/SporkUnrollPass.cpp Makefile
	$(MAKE) -C gempass/build

fablepass/build/SporkUnroll.so: fablepass/SporkUnrollPass.cpp Makefile
	cmake -S fablepass -B fablepass/build -DLLVM_DIR="$(CURDIR)/llvm-project/build/lib/cmake/llvm" -DCMAKE_BUILD_TYPE=Release
	$(MAKE) -C fablepass/build

llvmtest: scheduler.cpp *.hpp gempass/build/SporkUnroll.so Makefile
#	$(SETPATH) $(CLANGPP) $(INCLUDE) $(OPTIONS) $(LIBRARY) $(DBGFLAG) $(LLVMOPT) -O3 $< -o $@
	$(SETPATH) $(CLANGPP) $(INCLUDE) $(OPTIONS) $(LIBRARY) $(DBGFLAG) -emit-llvm -S -O3 $< -o llvmtest.ll
	$(SETPATH) $(OPTLLVM) -load-pass-plugin=gempass/build/SporkUnroll.so  llvmtest.ll -o llvmtest.spork-unrolled.ll
	$(SETPATH) $(OPTLLVM) -O3 llvmtest.spork-unrolled.ll -o llvmtest.opt.ll
	$(SETPATH) $(CLANGPP) $(LIBRARY) $(DBGFLAG) llvmtest.opt.ll -o $@

.PHONY: clean
clean:
	rm scheduler schedulerO1 schedulerO2 schedulerO3 scheduler.ll scheduler.opt.ll scheduler.i schedulerO1.s schedulerO2.s schedulerO3.s
