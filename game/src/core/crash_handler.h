#pragma once

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>

#if defined(__APPLE__)
	#include <sys/ucontext.h>	// <ucontext.h> is the deprecated routines; the struct lives here
	#include <mach-o/dyld.h>
	#include <spawn.h>
	#include <sys/wait.h>
	extern char** environ;
#endif
#if defined(__linux__)
	#include <ucontext.h>
#endif

// Prints a callstack on fatal signals (crash, assert/abort, trap) and on
// fatal_exit(). Everything goes to stderr, which is unbuffered.
// Not strictly async-signal-safe (dladdr, posix_spawn); acceptable on a path
// whose only job is to report before the process dies anyway.

// Symbolizes the game's own frames to file:line with atos, which reads the
// binary and its debug map (the -g object file) without attaching.
// shortcut: Linux prints function names only; add addr2line if Linux crashes need lines.
// in_first_is_pc: frame 0 is an exact faulting PC; every other frame is a
// return address, looked up at address - 1 so a call that never returns (at
// the very end of a function) is not attributed to the next line or function.
inline void crash_print_source_lines(void* const* in_frames, const int in_count, const bool in_first_is_pc)
{
#if defined(__APPLE__)
	const void* main_image = _dyld_get_image_header(0);
	char executable_path[4096];
	uint32_t executable_path_size = sizeof(executable_path);
	if (_NSGetExecutablePath(executable_path, &executable_path_size) != 0)
	{
		return;
	}

	char load_address[32];
	snprintf(load_address, sizeof(load_address), "%p", main_image);
	static char addresses[128][32];
	const char* argv[128 + 8] = { "/usr/bin/atos", "-o", executable_path, "-l", load_address };
	const int fixed_argc = 5;
	int argc = fixed_argc;
	for (int i = 0; i < in_count && i < 128; ++i)
	{
		Dl_info info = {};
		if (dladdr(in_frames[i], &info) && info.dli_fbase == main_image)
		{
			const bool is_return_address = i > 0 || !in_first_is_pc;
			snprintf(addresses[i], sizeof(addresses[i]), "%p", (char*) in_frames[i] - (is_return_address ? 1 : 0));
			argv[argc++] = addresses[i];
		}
	}
	argv[argc] = nullptr;
	if (argc == fixed_argc)
	{
		return;
	}

	fprintf(stderr, "---- game frames (file:line) ----\n");
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
	pid_t pid = 0;
	if (posix_spawn(&pid, argv[0], &actions, nullptr, (char* const*) argv, environ) == 0)
	{
		int status = 0;
		waitpid(pid, &status, 0);
	}
	posix_spawn_file_actions_destroy(&actions);
#else
	(void) in_frames;
	(void) in_count;
	(void) in_first_is_pc;
#endif
}

// in_skip_frames counts frames above the caller of interest, starting with
// this function; noinline on the callers keeps that count fixed across
// optimization levels. in_fault_pc: the faulting instruction for a signal. A
// frame-pointer walk started inside the handler loses that frame (the caller
// shows up twice instead), so it is put back here.
__attribute__((noinline)) inline void crash_print_callstack(const int in_skip_frames, void* in_fault_pc = nullptr)
{
	void* frames[129];
	int count = backtrace(frames, 128);
	int skip = in_skip_frames < count ? in_skip_frames : 0;
	const bool first_is_pc = in_fault_pc != nullptr;
	if (in_fault_pc && skip < count && frames[skip] != in_fault_pc)
	{
		if (skip + 1 < count && frames[skip] == frames[skip + 1])
		{
			frames[skip] = in_fault_pc;
		}
		else
		{
			memmove(frames + skip + 1, frames + skip, sizeof(void*) * (count - skip));
			frames[skip] = in_fault_pc;
			count += 1;
		}
	}
	fprintf(stderr, "---- callstack ----\n");
	backtrace_symbols_fd(frames + skip, count - skip, STDERR_FILENO);
	crash_print_source_lines(frames + skip, count - skip, first_is_pc);
	fprintf(stderr, "-------------------\n");
}

// For deliberate fatal exits (VK_CHECK and friends): same report, same exit code.
[[noreturn]] __attribute__((noinline)) inline void fatal_exit(const int in_code)
{
	fprintf(stderr, "\n==== FATAL EXIT (%d) ====\n", in_code);
	crash_print_callstack(2);	// this function + crash_print_callstack
	exit(in_code);
}

inline void* crash_fault_pc(void* in_context)
{
	if (!in_context) return nullptr;
	const ucontext_t* context = (const ucontext_t*) in_context;
#if defined(__APPLE__) && defined(__arm64__)
	return (void*) context->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
	return (void*) context->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__x86_64__)
	return (void*) context->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
	return (void*) context->uc_mcontext.pc;
#else
	return nullptr;
#endif
}

__attribute__((noinline)) inline void crash_signal_handler(const int in_signal, siginfo_t* in_info, void* in_context)
{
	fprintf(stderr, "\n==== FATAL SIGNAL %d (%s) ====\n", in_signal, strsignal(in_signal));
	if (in_signal == SIGSEGV || in_signal == SIGBUS)
	{
		fprintf(stderr, "faulting address: %p\n", in_info ? in_info->si_addr : nullptr);
	}
	// Skip crash_print_callstack, this handler and the signal trampoline.
	// abort/raise/trap report the raising call itself, so only hardware faults
	// need the PC put back.
	const bool hardware_fault = in_signal == SIGSEGV || in_signal == SIGBUS
		|| in_signal == SIGILL || in_signal == SIGFPE;
	crash_print_callstack(3, hardware_fault ? crash_fault_pc(in_context) : nullptr);
	// SA_RESETHAND restored the default action; re-raise so the exit status and
	// any OS crash report still reflect the original signal.
	raise(in_signal);
}

inline void crash_handler_install()
{
	// Own stack so a stack overflow can still run the handler.
	static char alternate_stack[256 * 1024];
	stack_t stack = {};
	stack.ss_sp = alternate_stack;
	stack.ss_size = sizeof(alternate_stack);
	sigaltstack(&stack, nullptr);

	struct sigaction action = {};
	action.sa_sigaction = crash_signal_handler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	const int crash_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
	for (const int crash_signal : crash_signals)
	{
		sigaction(crash_signal, &action, nullptr);
	}
}
