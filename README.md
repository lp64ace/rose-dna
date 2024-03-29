This project cannot be built standalone, it requires LLVM and clang to build this.
This is a clang-tool so you can run it using clang tools

# How To Build

Download the files in `\llvm-project\clang-tools-extra\`.

A folder `\llvm-project\clang-tools-extra\rose-dna` should have been created!

Navigate to `\llvm-project\clang-tools-extra\` and edit the `CMakeLists.txt` file,
you need to add `add_subdirectory(rose-dna)` at the end of the file.

```
...
add_subdirectory(rose-dna)
```

Then you need to build llvm with `clang-tools-extra` activated.
