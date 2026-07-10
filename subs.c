/*
 * SUBS.C
 *
 * Copyright 1994 Matthew Dillon (dillon@apollo.backplane.com)
 * Copyright 2009-2019 James Pryor <dubiousjim@gmail.com>
 * May be distributed under the GNU General Public License version 2 or any later version.
 */

#include "defs.h"

Prototype void printlogf(int level, const char *ctl, ...) ATTR_PRINTF(2, 3);
Prototype void fdprintlogf(int level, int fd, const char *ctl, ...) ATTR_PRINTF(3, 4);
Prototype int fdprintf(int fd, const char *ctl, ...) ATTR_PRINTF(2, 3);
Prototype void initsignals(void);
Prototype char Hostname[SMALL_BUFFER];

static void vlog(int level, int fd, const char *ctl, va_list va);

char Hostname[SMALL_BUFFER];


void
printlogf(int level, const char *ctl, ...)
{
	va_list va;

	va_start(va, ctl);
	vlog(level, 2, ctl, va);
	va_end(va);
}

void
fdprintlogf(int level, int fd, const char *ctl, ...)
{
	va_list va;

	va_start(va, ctl);
	vlog(level, fd, ctl, va);
	va_end(va);
}

int
fdprintf(int fd, const char *ctl, ...)
{
	va_list va;
	char buf[LOG_BUFFER];
	ssize_t n, written = 0, len;

	va_start(va, ctl);
	vsnprintf(buf, sizeof(buf), ctl, va);
	va_end(va);

	len = (ssize_t)strlen(buf);
	while (written < len) {
		n = write(fd, buf + written, (size_t)(len - written));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return (int)written;
		}
		written += n;
	}
	return (int)written;
}

static void
vlog(int level, int fd, const char *ctl, va_list va)
{
	char buf[LOG_BUFFER];
	static short suppressHeader = 0;
	static short hostname_initialized = 0;

	if (level > LogLevel)
		return;

	if (ForegroundOpt) {
		/*
		 * when -d or -f, we always (and only) log to stderr.
		 * fd will be 2 except when 2 is bound to an exec'd subprocess,
		 * in which case it will be 8.
		 */
		vsnprintf(buf, sizeof(buf), ctl, va);
		if (write(fd, buf, strlen(buf)) < 0) {
			/* Ignore write errors to avoid cascading failures */
		}
	} else if (SyslogOpt) {
		/* log to syslog */
		vsnprintf(buf, sizeof(buf), ctl, va);
		syslog(level, "%s", buf);

	} else {
		/* log to file */

		time_t t = time(NULL);
		struct tm *tp = localtime(&t);
		int buflen, hdrlen = 0;

		buf[0] = '\0'; /* in case suppressHeader or strftime fails */

		if (!suppressHeader) {
			/*
			 * run LogHeader through strftime --> [yields hdr],
			 * then plug in Hostname --> [yields buf]
			 */
			char hdr[SMALL_BUFFER];

			/* Initialize hostname exactly once */
			if (!hostname_initialized) {
				(void)gethostname(Hostname, sizeof(Hostname));
				/* Always NUL-terminate regardless of gethostname result */
				Hostname[sizeof(Hostname) - 1] = '\0';
				hostname_initialized = 1;
			}

			/* strftime with a runtime format string — non-literal by design */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
			if (strftime(hdr, sizeof(hdr), LogHeader, tp)) {
				/* snprintf with a strftime-expanded format — non-literal by design */
				if ((hdrlen = snprintf(buf, sizeof(hdr), hdr, Hostname)) >= (int)sizeof(hdr))
					hdrlen = (int)sizeof(hdr) - 1;
			}
#pragma GCC diagnostic pop
		}

		if ((buflen = vsnprintf(buf + hdrlen, sizeof(buf) - (size_t)hdrlen, ctl, va) + hdrlen) >= (int)sizeof(buf))
			buflen = (int)sizeof(buf) - 1;

		if (write(fd, buf, (size_t)buflen) < 0) {
			/* Ignore write errors to avoid cascading failures */
		}
		/* if previous write wasn't \n-terminated, suppress header on next write */
		suppressHeader = (buf[buflen - 1] != '\n');
	}
}

static void
reopenlogger(int sig)
{
	int fd;
	(void)sig;
	if (getpid() == DaemonPid) {
		/* only the daemon handles this; children should ignore */
		if ((fd = open(LogFile, O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC, 0600)) < 0) {
			/* can't reopen log file, exit */
			exit(errno);
		}
		dup2(fd, 2);
		close(fd);
	}
}

static void
waitmailjob(int sig)
{
	pid_t child;
	(void)sig;

	/* Guard against DaemonPid being uninitialized (0 would kill the process group) */
	if (DaemonPid <= 0)
		return;

	/*
	 * Wait for any children in our process group — these are all mailjobs.
	 */
	do {
		child = waitpid(-DaemonPid, NULL, WNOHANG);
	} while (child > (pid_t)0);
}

static void
quit(int sig)
{
	(void)sig;
	Quit = 1;
}

void
initsignals(void)
{
	struct sigaction sa;
	int n;

	/* save daemon's pid globally */
	DaemonPid = getpid();

	sigemptyset(&sa.sa_mask);

	/* restart any system calls that were interrupted by signal */
	sa.sa_flags = SA_RESTART;
	sa.sa_handler = (!ForegroundOpt && !SyslogOpt) ? reopenlogger : SIG_IGN;
	if (sigaction(SIGHUP, &sa, NULL) != 0) {
		n = errno;
		fdprintf(2, "failed to start SIGHUP handling, reason: %s", strerror(errno));
		exit(n);
	}

	sa.sa_flags = SA_RESTART;
	sa.sa_handler = quit;
	if (sigaction(SIGINT, &sa, NULL) != 0) {
		n = errno;
		fdprintf(2, "failed to start SIGINT handling, reason: %s", strerror(errno));
		exit(n);
	}

	sa.sa_flags = SA_RESTART;
	sa.sa_handler = quit;
	if (sigaction(SIGTERM, &sa, NULL) != 0) {
		n = errno;
		fdprintf(2, "failed to start SIGTERM handling, reason: %s", strerror(errno));
		exit(n);
	}

	sa.sa_flags = SA_RESTART;
	sa.sa_handler = waitmailjob;
	if (sigaction(SIGCHLD, &sa, NULL) != 0) {
		n = errno;
		fdprintf(2, "failed to start SIGCHLD handling, reason: %s", strerror(errno));
		exit(n);
	}

	sa.sa_flags = SA_RESTART;
	sa.sa_handler = quit;
	if (sigaction(SIGQUIT, &sa, NULL) != 0) {
		n = errno;
		fdprintf(2, "failed to start SIGQUIT handling, reason: %s", strerror(errno));
		exit(n);
	}
}
