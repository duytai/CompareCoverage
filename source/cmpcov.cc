/////////////////////////////////////////////////////////////////////////
//
// Author: Mateusz Jurczyk (mjurczyk@google.com)
//
// Copyright 2019 Google LLC
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
// https://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Description
// ===========
//
// A simple instrumentation module which implements the callbacks defined by the
// "tracing data flow" SanitizerCoverage feature:
//
// https://clang.llvm.org/docs/SanitizerCoverage.html#tracing-data-flow
//
// The purpose of the module is to extract information about the number of
// matching bytes in comparisons and switch/case statements, and dump it to disk
// in .sancov files, similarly to how "regular" code coverage information is
// saved by SanitizerCoverage when the target is compiled with the
// -fsanitize-coverage=trace-pc-guard flag. The output generated by this tool is
// complimentary to the basic edge-based coverage, and is meant to be used as a
// sub-instruction profiling instrument, which makes it possible for fuzzers to
// progress through 16/32/64-bit constants and textual strings expected in the
// input stream. For reference, see e.g.:
//
// 1) http://taviso.decsystem.org/making_software_dumber.pdf
// 2) https://lafintel.wordpress.com/2016/08/15/circumventing-fuzzing-roadblocks-with-compiler-transformations/
//
// Building
// ========
//
// Compile your fuzzing target with the following flags:
//
// -fsanitize=address -fsanitize-coverage=trace-pc-guard,trace-cmp
//
// and link this library into it.
//
// Runtime configuration
// =====================
//
// The default behavior (instrumenting 2/4/8-byte comparisons with constant
// values and switch/case constructs) is enabled the same way the overall
// SanitizerCoverage is enabled, i.e. ASAN_OPTIONS=coverage=1. The output
// directory for the .sancov log is also controlled by the standard
// ASAN_OPTIONS=coverage_dir=... flag. Additional options are configured by
// custom variables:
//
// TRACE_NONCONST_CMP - enables the tracing of comparisons where neither of the
//                      operands is constant.
//
// TRACE_MEMORY_CMP   - enables the tracing of memcmp(), strcmp() and similar
//                      functions.
//

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif __linux__
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "common.h"
#include "modules.h"
#include "tokenizer.h"
#include "traces.h"

#if defined(__SANITIZE_ADDRESS__) ||\
    (defined(__has_feature) && __has_feature(address_sanitizer))
#error The cmpcov module should be compiled separately to the fuzzing target,\
       without AddressSanitizer enabled, to avoid infinite recursions and other\
       problems. Compile cmpcov to object files first, and then include them in\
       your project.
#endif

#if !defined(_WIN32) && !defined(__linux__)
#error Unsupported operating system.
#endif

struct Configuration {
  // Indicates if the overall cmpcov instrumentation is enabled, as configured
  // through the standard ASAN_OPTIONS environment variable:
  //
  // ASAN_OPTIONS=coverage=1
  //
  // Default: false
  bool enabled;

  // Indicates if non-constant cmp instrumentation is enabled, as configured by
  // the TRACE_NONCONST_CMP variable, e.g.:
  //
  // ASAN_OPTIONS=coverage=1 TRACE_NONCONST_CMP=1
  //
  // Default: false
  bool nonconst_cov_enabled;

  // Indicates if memory-comparison functions such as memcpy(), strcmp() etc.
  // are instrumented by the module, as configured by the TRACE_MEMORY_CMP
  // variable, e.g.:
  //
  // ASAN_OPTIONS=coverage=1 TRACE_MEMORY_CMP=0
  //
  // Default: true
  bool memory_cov_enabled;

  // Stores the output directory path for the *.sancov files produced by the
  // instrumentation. It is configured through the coverage_dir switch in the
  // ASAN_OPTIONS environment variable:
  //
  // ASAN_OPTIONS=coverage=1,coverage_dir=/path/to/directory
  //
  // Default: "." (current directory)
  std::string coverage_dir;
};

namespace globals {
  // A global mutex guarding access to all of the variables and objects below.
  static std::mutex cov_mutex;

  // Indicates if the cmpcov module has been initialized yet.
  static bool initialized;

  // A pointer to an object storing globally-accessible internal structures.
  // These structures are not destroyed before the death of the process.
  static Configuration *config;

  // An internal class storing information about all execution traces registered
  // so far.
  static Traces *traces;
}  // namespace globals

////////////////////////////////////////////////////////////////////////////////
//
// Helper functions.
//
////////////////////////////////////////////////////////////////////////////////

static void ParseAsanConfig() {
  const char *asan_options_ptr = getenv("ASAN_OPTIONS");
  if (asan_options_ptr == nullptr) {
    return;
  }

  std::string asan_options(asan_options_ptr);
  std::vector<std::pair<std::string, std::string>> tokens;
  if (!TokenizeString(asan_options, &tokens)) {
    Die("Unable to parse the ASAN_OPTIONS environment variable.\n");
  }

  for (const auto& it : tokens) {
    if (it.first == "coverage") {
      globals::config->enabled = (atoi(it.second.c_str()) != 0);
    } else if (it.first == "coverage_dir") {
      globals::config->coverage_dir = it.second;
    }
  }

  const char *nonconst_cmp_ptr = getenv("TRACE_NONCONST_CMP");
  if (nonconst_cmp_ptr != nullptr) {
    globals::config->nonconst_cov_enabled = (atoi(nonconst_cmp_ptr) != 0);
  }

  const char *memory_cmp_ptr = getenv("TRACE_MEMORY_CMP");
  if (memory_cmp_ptr != nullptr) {
    globals::config->memory_cov_enabled = (atoi(memory_cmp_ptr) == 0);
  }
}

static void DumpCoverageOnExit() {
  std::lock_guard<std::mutex> lock(globals::cov_mutex);

  struct OutputFileDescriptor {
    FILE *file;
    int counter;
    char path[MAX_PATH];
  };
  std::vector<OutputFileDescriptor> output_files(
      globals::traces->GetModulesCount());

  std::vector<std::pair<int, size_t>> traces_list;
  globals::traces->GetTracesList(&traces_list);

  for (const auto& trace : traces_list) {
    const int mod_idx = trace.first;
    auto& output_file = output_files[mod_idx];
    FILE *f = output_file.file;

    if (f == nullptr) {
      snprintf(output_file.path, sizeof(output_file.path),
               "%s/cmp.%s.%d.sancov",
               globals::config->coverage_dir.c_str(),
               globals::traces->GetModuleName(mod_idx).c_str(),
               GetPid());

      f = fopen(output_file.path, "w+b");
      if (f == nullptr) {
        Die("Unable to open the \"%s\" file for writing.\n", output_file.path);
      }

      fwrite(&kMagic, sizeof(kMagic), 1, f);

      output_file.file = f;
    }

    fwrite(&trace.second, sizeof(size_t), 1, f);

    output_file.counter++;
  }

  for (const auto& output_file : output_files) {
    if (output_file.file != nullptr) {
      fprintf(stderr, "CmpSanitizerCoverage: %s: %d PCs written\n",
              output_file.path, output_file.counter);

      fclose(output_file.file);
    }
  }
}

static void Initialize() {
  // Bail out if already initialized.
  if (globals::initialized) {
    return;
  }

  // Allocate the configuration and traces objects.
  globals::config = new Configuration;
  globals::traces = new Traces;

  // Set up some sane defaults.
  globals::config->enabled = false;
  globals::config->nonconst_cov_enabled = false;
  globals::config->memory_cov_enabled = true;
  globals::config->coverage_dir = ".";

  // Initialize the configuration data based on the ASAN_OPTIONS variable.
  ParseAsanConfig();

  // Register a destructor to save output data if the instrumentation is
  // enabled.
  if (globals::config->enabled) {
    atexit(DumpCoverageOnExit);
  }

  globals::initialized = true;
}

static size_t InternalStrnlen(const char *s, size_t max_length) {
  size_t len = 0;
  for (; len < max_length && s[len] != '\0'; len++) { }
  return len;
}

static size_t InternalStrnlen2(
    const char *s1, const char *s2, size_t max_length) {
  size_t len = 0;
  for (; len < max_length && s1[len] != '\0' && s2[len] != '\0'; len++) { }
  return len;
}

static int GetUint32Width(uint32_t x) {
  return (32 - (__builtin_clz(x) & (~7))) / 8;
}

static int GetUint64Width(uint64_t x) {
  return (64 - (__builtin_clzll(x) & (~7))) / 8;
}

static int CountMatchingBytes(int count, uint64_t x, uint64_t y) {
  int i;
  for (i = 0; i < count; i++) {
    if (((x >> (i * 8)) & 0xff) != ((y >> (i * 8)) & 0xff)) {
      break;
    }
  }
  return i;
}

static void CommonHandleCmpTrace(uint64_t arg1, uint64_t arg2, int arg_length,
                                 int switch_case, void *pc) {
  const int matching_bytes = CountMatchingBytes(arg_length, arg1, arg2);
  for (int i = 1; i <= matching_bytes; i++) {
    globals::traces->TrySaveTrace(reinterpret_cast<size_t>(pc),
                                  /*trace_arg1=*/i, /*trace_arg2=*/switch_case);
  }
}

static void CommonHandleMemcmpTrace(const char *s1, const char *s2, int length,
                                    void *pc) {
  int matching_bytes;
  for (matching_bytes = 0; matching_bytes < length; matching_bytes++) {
    if (*s1++ != *s2++) {
      break;
    }
  }

  for (int i = 1; i <= matching_bytes; i++) {
    globals::traces->TrySaveTrace(reinterpret_cast<size_t>(pc),
                                  /*trace_arg1=*/kMemcmpTraceArg1,
                                  /*trace_arg2=*/i);
  }
}

////////////////////////////////////////////////////////////////////////////////
//
// Declarations of SanitizerCoverage callback functions invoked before every
// executed comparison in the program.
//
////////////////////////////////////////////////////////////////////////////////

extern "C" {

void __sanitizer_cov_trace_cmp1(uint8_t Arg1, uint8_t Arg2) {
  // Single-byte comparisons are not instrumented. It is assumed that fuzzers
  // typically operate on byte granularity and are able to eventually guess the
  // value of a byte in the input stream. Performing cmp instrumentation on a
  // bit-level could unnecessarily bloat the size of the coverage information
  // log.
}

void __sanitizer_cov_trace_cmp2(uint16_t Arg1, uint16_t Arg2) {
  std::lock_guard<std::mutex> lock(globals::cov_mutex);

  if (!globals::initialized) {
    Initialize();
  }
  if (!globals::config->enabled || !globals::config->nonconst_cov_enabled) {
    return;
  }

  CommonHandleCmpTrace(Arg1, Arg2, /*arg_length=*/2, /*switch_case=*/0,
                       __builtin_return_address(0));
}

void __sanitizer_cov_trace_cmp4(uint32_t Arg1, uint32_t Arg2) {
  std::lock_guard<std::mutex> lock(globals::cov_mutex);

  if (!globals::initialized) {
    Initialize();
  }
  if (!globals::config->enabled || !globals::config->nonconst_cov_enabled) {
    return;
  }

  CommonHandleCmpTrace(Arg1, Arg2, /*arg_length=*/4, /*switch_case=*/0,
                       __builtin_return_address(0));
}

void __sanitizer_cov_trace_cmp8(uint64_t Arg1, uint64_t Arg2) {
  std::lock_guard<std::mutex> lock(globals::cov_mutex);

  if (!globals::initialized) {
    Initialize();
  }
  if (!globals::config->enabled || !globals::config->nonconst_cov_enabled) {
    return;
  }

  CommonHandleCmpTrace(Arg1, Arg2, /*arg_length=*/8, /*switch_case=*/0,
                       __builtin_return_address(0));
}

void __sanitizer_cov_trace_const_cmp1(uint8_t Arg1, uint8_t Arg2) {
  // Single-byte comparisons are not instrumented. It is assumed that fuzzers
  // typically operate on byte granularity and are able to eventually guess the
  // value of a byte in the input stream. Performing cmp instrumentation on a
  // bit-level could unnecessarily bloat the size of the coverage information
  // log.
}

void __sanitizer_cov_trace_const_cmp2(uint16_t Arg1, uint16_t Arg2) {
  // Quick initial check if the constant value is wider than a single byte. If
  // not, we skip the instrumentation of the comparison.
  if (Arg1 < 0x100) {
    return;
  }

  std::lock_guard<std::mutex> lock(globals::cov_mutex);

  if (!globals::initialized) {
    Initialize();
  }
  if (!globals::config->enabled) {
    return;
  }

  CommonHandleCmpTrace(Arg1, Arg2, /*arg_length=*/2, /*switch_case=*/0,
                       __builtin_return_address(0));
}

void __sanitizer_cov_trace_const_cmp4(uint32_t Arg1, uint32_t Arg2) {
  // Quick initial check if the constant value is wider than a single byte. If
  // not, we skip the instrumentation of the comparison.
  if (Arg1 < 0x100) {
    return;
  }

  std::lock_guard<std::mutex> lock(globals::cov_mutex);

  if (!globals::initialized) {
    Initialize();
  }
  if (!globals::config->enabled) {
    return;
  }

  CommonHandleCmpTrace(Arg1, Arg2,
                       /*arg_length=*/GetUint32Width(Arg1), /*switch_case=*/0,
                       __builtin_return_address(0));
}

void __sanitizer_cov_trace_const_cmp8(uint64_t Arg1, uint64_t Arg2) {
  // Quick initial check if the constant value is wider than a single byte. If
  // not, we skip the instrumentation of the comparison.
  if (Arg1 < 0x100) {
    return;
  }

  std::lock_guard<std::mutex> lock(globals::cov_mutex);

  if (!globals::initialized) {
    Initialize();
  }
  if (!globals::config->enabled) {
    return;
  }

  CommonHandleCmpTrace(Arg1, Arg2,
                       /*arg_length=*/GetUint64Width(Arg1), /*switch_case=*/0,
                       __builtin_return_address(0));
}

void __sanitizer_cov_trace_switch(uint64_t Val, uint64_t *Cases) {
  // If there are no cases in the switch() construct (possibly because we
  // overwrote it earlier to prevent this switch from being processed again),
  // return immediately.
  if (Cases[0] == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(globals::cov_mutex);

  if (!globals::initialized) {
    Initialize();
  }
  if (!globals::config->enabled) {
    return;
  }

  // From SanitizerCoverage documentation:
  //
  // Val is the switch operand.
  // Cases[0] is the number of case constants.
  // Cases[1] is the size of Val in bits.
  // Cases[2:] are the case constants.
  bool wide_value_found = false;
  for (int i = 0; i < Cases[0]; i++) {
    // Similarly to regular cmp instructions, we're not interested in
    // instrumenting switch cases with 1-byte constants.
    if (Cases[2 + i] < 0x100) {
      continue;
    }

    wide_value_found = true;

    CommonHandleCmpTrace(/*arg1=*/Val,
                         /*arg2=*/Cases[2 + i],
                         /*arg_length=*/GetUint64Width(Cases[2 + i]),
                         /*switch_case=*/i + 1,
                         __builtin_return_address(0));
  }

  // This optimization is based on the fact that the Cases[] arrays are placed
  // by ASAN in r/w memory. We have noticed that 8/16-bit switch() constructs
  // are rounded up to 32 bits, and that a majority of such constructs don't
  // operate on "wide" constants which this instrumentation is supposed to help
  // with.
  //
  // If we notice a switch() that doesn't have a single interesting constant, we
  // zero out the number of its elements so that we don't redundantly process it
  // in the future.
  if (!wide_value_found) {
    Cases[0] = 0;
  }
}

void __sanitizer_cov_trace_div4(uint32_t Val) {
  // Division operations are not instrumented, as we don't believe they carry
  // a significant amount of useful information.
}

void __sanitizer_cov_trace_div8(uint64_t Val) {
  // Division operations are not instrumented, as we don't believe they carry
  // a significant amount of useful information.
}

void __sanitizer_cov_trace_gep(uintptr_t Idx) {
  // Not instrumented.
}

void __sanitizer_weak_hook_memcmp(void *caller_pc, const void *s1,
                                  const void *s2, size_t n, int result) {
  // Ignore too too long data comparisons.
  if (n > kMaxDataCmpLength) {
    return;
  }

  // Try to acquire the lock; if the attempt fails, it's most likely a reentry
  // situation and we should return.
  //
  // A reentry could occur while performing string operations in our __sanitizer
  // instrumentation callbacks. We don't want to instrument memcmp() and similar
  // functions invoked by cmpcov itself.
  std::unique_lock<std::mutex> lock(globals::cov_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }

  if (!globals::initialized) {
    Initialize();
  }
  if (!globals::config->enabled || !globals::config->memory_cov_enabled) {
    return;
  }

  CommonHandleMemcmpTrace(static_cast<const char *>(s1),
                          static_cast<const char *>(s2),
                          /*length=*/n, caller_pc);
}

void __sanitizer_weak_hook_strncmp(void *caller_pc, const char *s1,
                                   const char *s2, size_t n, int result) {
  // Ignore too long data comparisons.
  if (n > kMaxDataCmpLength) {
    return;
  }

  // Try to acquire the lock; if the attempt fails, it's most likely a reentry
  // situation and we should return.
  std::unique_lock<std::mutex> lock(globals::cov_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }

  if (!globals::initialized) {
    Initialize();
  }
  if (!globals::config->enabled || !globals::config->memory_cov_enabled) {
    return;
  }

  // This is effectively:
  //
  // n = min(n, strlen(s1), strlen(s2))
  n = InternalStrnlen(s1, n);
  n = InternalStrnlen(s2, n);

  CommonHandleMemcmpTrace(s1, s2, /*length=*/n, caller_pc);
}

void __sanitizer_weak_hook_strcmp(void *caller_pc, const char *s1,
                                  const char *s2, int result) {
  // Try to acquire the lock; if the attempt fails, it's most likely a reentry
  // situation and we should return.
  std::unique_lock<std::mutex> lock(globals::cov_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }

  if (!globals::initialized) {
    Initialize();
  }
  if (!globals::config->enabled || !globals::config->memory_cov_enabled) {
    return;
  }

  // Calculate min(strlen(s1), strlen(s2)). If both strings are longer than
  // kMaxDataCmpLength, it's most likely not a comparison we're interested in.
  const size_t n = InternalStrnlen2(s1, s2, kMaxDataCmpLength + 1);
  if (n > kMaxDataCmpLength) {
    return;
  }

  CommonHandleMemcmpTrace(s1, s2, /*length=*/n, caller_pc);
}

void __sanitizer_weak_hook_strncasecmp(void *called_pc, const char *s1,
                                       const char *s2, size_t n, int result) {
  return __sanitizer_weak_hook_strncmp(called_pc, s1, s2, n, result);
}

void __sanitizer_weak_hook_strcasecmp(void *called_pc, const char *s1,
                                      const char *s2, int result) {
  return __sanitizer_weak_hook_strcmp(called_pc, s1, s2, result);
}

}  // extern "C"
