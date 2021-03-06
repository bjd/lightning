#include <assert.h>
#include <backtrace-supported.h>
#include <backtrace.h>
#include <ccan/err/err.h>
#include <ccan/io/io.h>
#include <ccan/str/str.h>
#include <common/daemon.h>
#include <common/status.h>
#include <common/utils.h>
#include <common/version.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <wally_core.h>

struct backtrace_state *backtrace_state;

#if BACKTRACE_SUPPORTED
static void (*bt_print)(const char *fmt, ...) PRINTF_FMT(1,2);
static void (*bt_exit)(void);

static int backtrace_status(void *unused UNUSED, uintptr_t pc,
			    const char *filename, int lineno,
			    const char *function)
{
	bt_print("backtrace: %s:%d (%s) %p",
		 filename, lineno, function, (void *)pc);
	return 0;
}

static void crashdump(int sig)
{
	/* We do stderr first, since it's most reliable. */
	warnx("Fatal signal %d (version %s)", sig, version());
	if (backtrace_state)
		backtrace_print(backtrace_state, 0, stderr);

	/* Now send to parent. */
	bt_print("FATAL SIGNAL %d (version %s)", sig, version());
	if (backtrace_state)
		backtrace_full(backtrace_state, 0, backtrace_status, NULL, NULL);

	/* Probably shouldn't return. */
	bt_exit();

	/* This time it will kill us instantly. */
	kill(getpid(), sig);
}

static void crashlog_activate(void)
{
	struct sigaction sa;

	sa.sa_handler = crashdump;
	sigemptyset(&sa.sa_mask);

	/* We want to fall through to default handler */
	sa.sa_flags = SA_RESETHAND;
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
}
#endif

int daemon_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	const char *t;

	t = taken_any();
	if (t)
		errx(1, "Outstanding taken pointers: %s", t);

	clean_tmpctx();

	return poll(fds, nfds, timeout);
}

#if DEVELOPER
static void steal_notify(tal_t *child, enum tal_notify_type n, tal_t *newparent)
{
	tal_t *p = newparent;

	assert(tal_parent(child) == newparent);
	while ((p = tal_parent(p)) != NULL)
		assert(p != child);
}

static void add_steal_notifier(tal_t *parent UNUSED,
			       enum tal_notify_type type UNNEEDED,
			       void *child)
{
	tal_add_notifier(child, TAL_NOTIFY_ADD_CHILD, add_steal_notifier);
	tal_add_notifier(child, TAL_NOTIFY_STEAL, steal_notify);
}

static void add_steal_notifiers(const tal_t *root)
{
	tal_add_notifier(root, TAL_NOTIFY_ADD_CHILD, add_steal_notifier);

	for (const tal_t *i = tal_first(root); i; i = tal_next(i))
		add_steal_notifiers(i);
}
#endif

void daemon_setup(const char *argv0,
		  void (*backtrace_print)(const char *fmt, ...),
		  void (*backtrace_exit)(void))
{
	err_set_progname(argv0);

#if BACKTRACE_SUPPORTED
	bt_print = backtrace_print;
	bt_exit = backtrace_exit;
#if DEVELOPER
	/* Suppresses backtrace (breaks valgrind) */
	if (!getenv("LIGHTNINGD_DEV_NO_BACKTRACE"))
		backtrace_state = backtrace_create_state(argv0, 0, NULL, NULL);
	add_steal_notifiers(NULL);
#else
	backtrace_state = backtrace_create_state(argv0, 0, NULL, NULL);
#endif
	crashlog_activate();
#endif

	/* We handle write returning errors! */
	signal(SIGPIPE, SIG_IGN);
	wally_init(0);
	secp256k1_ctx = wally_get_secp_context();

	setup_tmpctx();
	io_poll_override(daemon_poll);
}

void daemon_shutdown(void)
{
	tal_free(tmpctx);
	wally_cleanup(0);
}
