CLANGPP := ~/code/llvm-project/build/bin/clang++
OPTLLVM := ~/code/llvm-project/build/bin/opt
INCLUDE := -I./parlaylib/include/ -I/usr/include/libunwind/
LIBRARY := -lunwind -lpthread
OPTIONS := -xc++ -stdlib=libc++ -std=c++17

scheduler: scheduler.cpp
	$(CLANGPP) $(INCLUDE) $(LIBRARY) $(OPTIONS) -ggdb $< -o $@

scheduler.o: scheduler.cpp
	$(CLANGPP) $(INCLUDE) $(OPTIONS) -S $< -o $@

scheduler.ll: scheduler.cpp
	$(CLANGPP) $(INCLUDE) $(OPTIONS) -S -emit-llvm $< -o $@

scheduler.opt.ll: scheduler.ll
	$(OPTLLVM) -S -passes=mem2reg $< -o $@

.PHONY: clean
clean:
	rm scheduler scheduler.ll scheduler.opt.ll
