set(LLVM_LINK_COMPONENTS support)

set(SRC
	src/main.cpp
)

add_clang_executable(rose-dna ${SRC})

target_link_libraries(rose-dna
	PRIVATE
	clangAST
	clangASTMatchers
	clangBasic
	clangFrontend
	clangSerialization
	clangTooling
)