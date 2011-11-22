#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

/* required for sandboxing */
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#	include <windows.h>
#	include <io.h>
#	include <shellapi.h>
#	include <direct.h>

#	define _MAIN_CC __cdecl

#	define stat(path, st) _stat(path, st)
#	define mkdir(path, mode) _mkdir(path)
#	define chdir(path) _chdir(path)
#	define access(path, mode) _access(path, mode)
#	define strdup(str) _strdup(str)

#	ifndef __MINGW32__
#		pragma comment(lib, "shell32")
#		define strncpy(to, from, to_size) strncpy_s(to, to_size, from, _TRUNCATE)
#		define W_OK 02
#		define S_ISDIR(x) ((x & _S_IFDIR) != 0)
#		define mktemp_s(path, len) _mktemp_s(path, len)
#	endif
	typedef struct _stat STAT_T;
#else
#	include <sys/wait.h> /* waitpid(2) */
#	include <unistd.h>
#	define _MAIN_CC
	typedef struct stat STAT_T;
#endif

#include "clay.h"

static void fs_rm(const char *_source);
static void fs_copy(const char *_source, const char *dest);

static const char *
fixture_path(const char *base, const char *fixture_name);

struct clay_error {
	const char *test;
	int test_number;
	const char *suite;
	const char *file;
	int line_number;
	const char *error_msg;
	char *description;

	struct clay_error *next;
};

static struct {
	const char *active_test;
	const char *active_suite;

	int suite_errors;
	int total_errors;

	int test_count;

	struct clay_error *errors;
	struct clay_error *last_error;

	void (*local_cleanup)(void *);
	void *local_cleanup_payload;

	jmp_buf trampoline;
	int trampoline_enabled;
} _clay;

struct clay_func {
	const char *name;
	void (*ptr)(void);
};

struct clay_suite {
	const char *name;
	struct clay_func initialize;
	struct clay_func cleanup;
	const struct clay_func *tests;
	size_t test_count;
};

/* From clay_print_*.c */
static void clay_print_init(int test_count, int suite_count, const char *suite_names);
static void clay_print_shutdown(int test_count, int suite_count, int error_count);
static void clay_print_error(int num, const struct clay_error *error);
static void clay_print_ontest(const char *test_name, int test_number, int failed);
static void clay_print_onsuite(const char *suite_name);
static void clay_print_onabort(const char *msg, ...);

/* From clay_sandbox.c */
static void clay_unsandbox(void);
static int clay_sandbox(void);

/* Autogenerated test data by clay */
${clay_callbacks}

static const struct clay_suite _clay_suites[] = {
    ${clay_suites}
};

static size_t _clay_suite_count = ${clay_suite_count};
static size_t _clay_callback_count = ${clay_callback_count};

/* Core test functions */
static void
clay_run_test(
	const struct clay_func *test,
	const struct clay_func *initialize,
	const struct clay_func *cleanup)
{
	int error_st = _clay.suite_errors;

	_clay.trampoline_enabled = 1;

	if (setjmp(_clay.trampoline) == 0) {
		if (initialize->ptr != NULL)
			initialize->ptr();

		test->ptr();
	}

	_clay.trampoline_enabled = 0;

	if (_clay.local_cleanup != NULL)
		_clay.local_cleanup(_clay.local_cleanup_payload);

	if (cleanup->ptr != NULL)
		cleanup->ptr();

	_clay.test_count++;

	/* remove any local-set cleanup methods */
	_clay.local_cleanup = NULL;
	_clay.local_cleanup_payload = NULL;

	clay_print_ontest(
		test->name,
		_clay.test_count,
		(_clay.suite_errors > error_st)
	);
}

static void
clay_report_errors(void)
{
	int i = 1;
	struct clay_error *error, *next;

	error = _clay.errors;
	while (error != NULL) {
		next = error->next;
		clay_print_error(i++, error);
		free(error->description);
		free(error);
		error = next;
	}

	_clay.errors = _clay.last_error = NULL;
}

static void
clay_run_suite(const struct clay_suite *suite)
{
	const struct clay_func *test = suite->tests;
	size_t i;

	clay_print_onsuite(suite->name);

	_clay.active_suite = suite->name;
	_clay.suite_errors = 0;

	for (i = 0; i < suite->test_count; ++i) {
		_clay.active_test = test[i].name;
		clay_run_test(&test[i], &suite->initialize, &suite->cleanup);
	}
}

#if 0 /* temporarily disabled */
static void
clay_run_single(const struct clay_func *test,
	const struct clay_suite *suite)
{
	_clay.suite_errors = 0;
	_clay.active_suite = suite->name;
	_clay.active_test = test->name;

	clay_run_test(test, &suite->initialize, &suite->cleanup);
}
#endif

static void
clay_usage(const char *arg)
{
	printf("Usage: %s [options]\n\n", arg);
	printf("Options:\n");
//	printf("  -tXX\t\tRun only the test number XX\n");
	printf("  -sXX\t\tRun only the suite number XX\n");
	exit(-1);
}

static void
clay_parse_args(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; ++i) {
		char *argument = argv[i];
		char action;
		int num;

		if (argument[0] != '-')
			clay_usage(argv[0]);

		action = argument[1];
		num = strtol(argument + 2, &argument, 10);

		if (*argument != '\0' || num < 0)
			clay_usage(argv[0]);

		switch (action) {
		case 's':
			if ((size_t)num >= _clay_suite_count) {
				clay_print_onabort("Suite number %d does not exist.\n", num);
				exit(-1);
			}

			clay_run_suite(&_clay_suites[num]);
			break;

		default:
			clay_usage(argv[0]);
		}
	}
}

static int
clay_test(int argc, char **argv)
{
	clay_print_init(
		(int)_clay_callback_count,
		(int)_clay_suite_count,
		""
	);

	if (clay_sandbox() < 0) {
		clay_print_onabort("Failed to sandbox the test runner.\n");
		exit(-1);
	}

	if (argc > 1) {
		clay_parse_args(argc, argv);
	} else {
		size_t i;
		for (i = 0; i < _clay_suite_count; ++i)
			clay_run_suite(&_clay_suites[i]);
	}

	clay_print_shutdown(
		_clay.test_count,
		(int)_clay_suite_count,
		_clay.total_errors
	);

	clay_unsandbox();
	return _clay.total_errors;
}

void
clay__assert(
	int condition,
	const char *file,
	int line,
	const char *error_msg,
	const char *description,
	int should_abort)
{
	struct clay_error *error;

	if (condition)
		return;

	error = calloc(1, sizeof(struct clay_error));

	if (_clay.errors == NULL)
		_clay.errors = error;

	if (_clay.last_error != NULL)
		_clay.last_error->next = error;

	_clay.last_error = error;

	error->test = _clay.active_test;
	error->test_number = _clay.test_count;
	error->suite = _clay.active_suite;
	error->file = file;
	error->line_number = line;
	error->error_msg = error_msg;

	if (description != NULL)
		error->description = strdup(description);

	_clay.suite_errors++;
	_clay.total_errors++;

	if (should_abort) {
		if (!_clay.trampoline_enabled) {
			clay_print_onabort(
				"Fatal error: a cleanup method raised an exception.");
			exit(-1);
		}

		longjmp(_clay.trampoline, -1);
	}
}

void cl_set_cleanup(void (*cleanup)(void *), void *opaque)
{
	_clay.local_cleanup = cleanup;
	_clay.local_cleanup_payload = opaque;
}

${clay_modules}

int _MAIN_CC main(int argc, char *argv[])
{
    return clay_test(argc, argv);
}
