# CompareCoverage

CompareCoverage (*CmpCov* in short) is a simple instrumentation module for C/C++ programs and libraries, which extracts information about data comparisons taking place in the code at run time, and saves it to disk in the form of standard `.sancov` files. It is based on the [SanitizerCoverage](https://clang.llvm.org/docs/SanitizerCoverage.html) instrumentation available in the `clang` compiler, which itself is tightly related to [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html). Specifically, the library implements the instrumentation callbacks defined by the [Tracing data flow](https://clang.llvm.org/docs/SanitizerCoverage.html#tracing-data-flow) feature of SanitizerCoverage.

The tool works similarly to how "regular" code coverage information is saved by SanitizerCoverage when the target is compiled with the `-fsanitize-coverage=trace-pc-guard` flag. The output generated by this tool is complimentary to the basic edge-based coverage, and is meant to be used as a sub-instruction profiling instrument, which makes it possible for fuzzers to progress through 16/32/64-bit constants and textual strings expected in the input stream. For reference, see e.g.:

1. http://taviso.decsystem.org/making_software_dumber.pdf
2. https://lafintel.wordpress.com/2016/08/15/circumventing-fuzzing-roadblocks-with-compiler-transformations/

In various forms, similar instrumentation is employed in the [afl](http://lcamtuf.coredump.cx/afl/), [libFuzzer](https://llvm.org/docs/LibFuzzer.html) and [honggfuzz](https://github.com/google/honggfuzz) fuzzers. CompareCoverage may prove useful when coupled with custom, dedicated fuzzers outside of the above list.

## Building

Makefiles for both Windows and GNU/Linux are provided. The end result is a static library which can be linked the your target software.

**Note**: The library is written in C++. When linking with software written in C, it might be necessary to add an extra `-lstdc++` flag to the linker command line.

### Linux

On Linux, `libcmpcov.a` is generated as shown below:

```bash
$ make -f Makefile.linux
clang++ -c -o cmpcov.o cmpcov.cc -O2 -fPIC
clang++ -c -o common.o common.cc -O2 -fPIC
clang++ -c -o modules.o modules.cc -O2 -fPIC
clang++ -c -o tokenizer.o tokenizer.cc -O2 -fPIC
clang++ -c -o traces.o traces.cc -O2 -fPIC
ar cr libcmpcov.a cmpcov.o common.o modules.o tokenizer.o traces.o
$
```

To build a program with AddressSanitizer, SanitizerCoverage and CompareCoverage, add the `-fsanitize=address -fsanitize-coverage=trace-pc-guard,trace-cmp` flags to the compilation step (e.g. `CFLAGS` or `CXXFLAGS`), and `-fsanitize=address -Wl,--whole-archive -L/cmpcov/directory/path -lcmpcov -Wl,--no-whole-archive` to the linking step (e.g. `LDFLAGS`):


```bash
$ clang++ -c test.cc -o test.o -fsanitize=address -fsanitize-coverage=trace-pc-guard,trace-cmp
$ clang++ test.o -o test -fsanitize=address -Wl,--whole-archive -L../cmpcov -lcmpcov -Wl,--no-whole-archive
$
```

### Windows

Compilation of `cmpcov.lib` is achieved as follows:

```batch
>make -f Makefile.win
clang-cl -c -o cmpcov.o cmpcov.cc -O2 -Wno-deprecated-declarations
clang-cl -c -o common.o common.cc -O2 -Wno-deprecated-declarations
clang-cl -c -o modules.o modules.cc -O2 -Wno-deprecated-declarations
clang-cl -c -o tokenizer.o tokenizer.cc -O2 -Wno-deprecated-declarations
clang-cl -c -o traces.o traces.cc -O2 -Wno-deprecated-declarations
llvm-lib /out:cmpcov.lib cmpcov.o common.o modules.o tokenizer.o traces.o
>
```

To build the target software with the complete instrumentation, add the `-fsanitize=address -fsanitize-coverage=trace-pc-guard,trace-cmp` flags to the compiler command line, and `-fsanitize=address -L/cmpcov/directory/path -lcmpcov` in the linking stage, e.g.:

```batch
>clang++ -c test.cc -o test.o -fsanitize=address -fsanitize-coverage=trace-pc-guard,trace-cmp
>clang++ test.o -o test.exe -fsanitize=address -lcmpcov -L../cmpcov
>
```

## Usage

CmpCov is generally controlled by the same `ASAN_OPTIONS` environment variable as SanitizerCoverage, and it currently supports two flags: `coverage` and `coverage_dir`. For example, to enable dumping the coverage information to disk, and have it saved in the `logs` directory, you can start your tested program as follows:

```bash
$ ASAN_OPTIONS=coverage=1,coverage_dir=logs ./test <<< "The quick"
CmpSanitizerCoverage: logs/cmp.test.75048.sancov: 9 PCs written
SanitizerCoverage: logs/test.75048.sancov: 2 PCs written
$ ls logs/
cmp.test.75048.sancov  test.75048.sancov
$
```

The test program above expected the "The quick brown fox ..." string on standard input, and because we provided a few of the first valid bytes, some comparison traces were generated and saved in an extra log file with a name starting with `cmp`. The more matching bytes there are at the beginning of a memory buffer or variable, the more traces are generated. The format of the output files is equivalent to that of typical `.sancov` files, and consists of a 64-bit header denoting the width of subsequent items (32/64-bit), followed by the traces themselves:

```bash
$ hexdump -C logs/test.75048.sancov
00000000  64 ff ff ff ff ff bf c0  81 e1 52 00 00 00 00 00  |d.........R.....|
00000010  7a e2 52 00 00 00 00 00                           |z.R.....|
00000018
$ hexdump -C logs/cmp.test.75048.sancov
00000000  64 ff ff ff ff ff bf c0  43 e2 12 00 00 00 01 f0  |d.......C.......|
00000010  43 e2 12 00 00 00 02 f0  43 e2 12 00 00 00 03 f0  |C.......C.......|
00000020  43 e2 12 00 00 00 04 f0  43 e2 12 00 00 00 05 f0  |C.......C.......|
00000030  43 e2 12 00 00 00 06 f0  43 e2 12 00 00 00 07 f0  |C.......C.......|
00000040  43 e2 12 00 00 00 08 f0  43 e2 12 00 00 00 09 f0  |C.......C.......|
00000050
$
```

In 64-bit mode, the lower 48 bits contain the instruction offset within the given module, while the upper 16 bits encode information about the comparison (type, switch/case index, number of matching bytes). In 32-bit mode, it is the same value, but hashed and truncated to 32 bits. For more details, please refer to the source code.

Additional `TRACE_NONCONST_CMP` and `TRACE_MEMORY_CMP` environment variables are available to control the instrumentation of non-const comparisons (off by default), and the instrumentation of memory/string functions (on by default).

The instrumentation was specifically designed to be compatible with the corpus management algorithm described in [Effective File Format Fuzzing](https://j00ru.vexillium.org/slides/2016/blackhat.pdf), but should work well with any other approach to corpus distillation.

## Example

To better illustrate the capabilities of CmpCov and tracing data flow in general, we developed a demonstration program [demo.cc](demo/demo.cc), which expects the following data on standard input:

* A "The quick brown fox " string checked with `memcmp`,
* A "jumps over " string checked with `strncmp`,
* A "the lazy dog" string checked with `strcmp`,
* A `0xCAFEBABECAFEBABE` 64-bit constant,
* A `0xDEADC0DE` 32-bit constant,
* A `0xBEEF` 16-bit constant.

Furthermore, we built a trivial [fuzzer](demo/fuzzer.py), which replaces subsequent bytes in the input stream with random values, until the coverage grows. A conventional fuzzer without any insight into the comparisons taking place wouldn't be able to progress through the checks. With CmpCov, all 57 bytes of input were successfully discovered in less than 4 minutes in our test run:

```bash
$ python fuzzer.py ./demo
---------- Initial coverage (2019-02-05 16:58:10, 2 traces) ----------
00000000: 26 3d 77 b7 bc bf 82 41 b4 a6 f2 c0 57 57 54 18 &=w....A....WWT.
00000010: 0c 29 01 72 e5 d4 a6 c0 ce bd b9 02 6c 87 24 48 .).r........l.$H
00000020: 7b 7d bb 34 08 60 5f 3a 0a 9a 06 ab f4 71 98 14 {}.4.`_:.....q..
00000030: 4c 84 e6 49 93 21 b0 2a 0d                      L..I.!.*.

[...]

---------- New coverage (2019-02-05 16:59:10, 24 traces) ----------
00000000: 54 68 65 20 71 75 69 63 6b 20 62 72 6f 77 6e 20 The quick brown
00000010: 66 6f 78 20 6a d4 a6 c0 ce bd b9 02 6c 87 24 48 fox j.......l.$H
00000020: 7b 7d bb 34 08 60 5f 3a 0a 9a 06 ab f4 71 98 14 {}.4.`_:.....q..
00000030: 4c 84 e6 49 93 21 b0 2a 0d                      L..I.!.*.

---------- New coverage (2019-02-05 16:59:14, 25 traces) ----------
00000000: 54 68 65 20 71 75 69 63 6b 20 62 72 6f 77 6e 20 The quick brown
00000010: 66 6f 78 20 6a 75 a6 c0 ce bd b9 02 6c 87 24 48 fox ju......l.$H
00000020: 7b 7d bb 34 08 60 5f 3a 0a 9a 06 ab f4 71 98 14 {}.4.`_:.....q..
00000030: 4c 84 e6 49 93 21 b0 2a 0d                      L..I.!.*.

[...]

---------- New coverage (2019-02-05 17:01:34, 65 traces) ----------
00000000: 54 68 65 20 71 75 69 63 6b 20 62 72 6f 77 6e 20 The quick brown
00000010: 66 6f 78 20 6a 75 6d 70 73 20 6f 76 65 72 20 74 fox jumps over t
00000020: 68 65 20 6c 61 7a 79 20 64 6f 67 be ba fe ca be he lazy dog.....
00000030: ba fe ca de c0 ad de ef be                      .........

$
```

## Disclaimer

This is not an official Google product.
