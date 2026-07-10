/*
 * JOB.C
 *
 * Copyright 1994 Matthew Dillon (dillon@apollo.backplane.com)
 * Copyright 2009-2019 James Pryor <dubiousjim@gmail.com>
 * May be distributed under the GNU General Public License version 2 or any later version.
 */

#include "defs.h"

Prototype void RunJob(CronFile *file, CronLine *line);
Prototype void EndJob(CronFile *file, CronLine *line, int exit_status);

Prototype const char *SendMail;

/*
 * Build the mail-file path into buf.  Uses a literal format string to
 * avoid -Wformat-nonliteral warnings that TempFileFmt would trigger.
 */
static void
make_mailfile(char *buf, size_t bufsz, const char *user, int pid)
{
	snprintf(buf, bufsz, "%s/cron.%s.%d", TempDir, user, pid);
}

void
RunJob(CronFile *file, CronLine *line)
{
	char mailFile[SMALL_BUFFER];
	int mailFd = -1;
	const char *value = Mailto;

	line->cl_Pid = 0;
	line->cl_MailFlag = 0;

	/*
	 * Try to open mail output file — owned by root so no unprivileged user
	 * can tamper with it before we send.
	 */

	make_mailfile(mailFile, sizeof(mailFile), file->cf_UserName, (int)getpid());

	mailFd = open(mailFile, O_CREAT|O_TRUNC|O_WRONLY|O_EXCL|O_APPEND|O_CLOEXEC, 0600);
	if (mailFd >= 0) {
		/* success: write email headers */
		line->cl_MailFlag = 1;
		/* if no -m Mailto was specified, deliver to the local user */
		if (!value)
			value = file->cf_UserName;
		fdprintf(mailFd, "To: %s\nSubject: cron for user %s %s\n\n",
				value,
				file->cf_UserName,
				line->cl_Description);
		/* record mail file size so we can detect whether the job produced output */
		line->cl_MailPos = lseek(mailFd, 0, SEEK_CUR);
	}
	/*
	 * If no mailFd, we'll complain after the fork and won't capture output,
	 * but we still run the job.
	 */

	/*
	 * Fork as the target user and run the command.
	 */

	line->cl_Pid = fork();

	if (line->cl_Pid == 0) {
		/*
		 * CHILD — change to target user, then exec the job
		 */

		if (ChangeUser(line->cl_UserName, TempDir) < 0) {
			printlogf(LOG_ERR, "unable to ChangeUser (user %s %s)\n",
					line->cl_UserName,
					line->cl_Description);
			_exit(1);
		}

		if (DebugOpt)
			printlogf(LOG_DEBUG, "child running: %s\n", line->cl_Description);

		/*
		 * Stash stderr (fd 2) in fd 8 as a close-on-exec log fd.
		 * This lets us write error messages even after the normal fds
		 * are redirected.
		 */
		dup2(2, 8);
		fcntl(8, F_SETFD, FD_CLOEXEC);
		fclose(stderr);

		if (mailFd >= 0) {
			/* stdin is already /dev/null; redirect stdout and stderr to the mail file */
			dup2(mailFd, 1);
			dup2(mailFd, 2);
			close(mailFd);
		} else {
			fdprintlogf(LOG_WARNING, 8,
					"unable to create mail file %s: cron output for user %s %s to /dev/null\n",
					mailFile,
					file->cf_UserName,
					line->cl_Description);
			dup2(1, 2);
		}

		/* New process group so mailjobs stay in the daemon's group, not here */
		setpgid(0, 0);

		execl("/bin/sh", "/bin/sh", "-c", line->cl_Shell, NULL);

		/* exec failed */
		fdprintlogf(LOG_ERR, 8, "unable to exec (user %s cmd /bin/sh -c %s)\n",
				line->cl_UserName,
				line->cl_Shell);
		fdprintf(1, "unable to exec: /bin/sh -c %s\n", line->cl_Shell);
		_exit(1);

	} else if (line->cl_Pid < 0) {
		/*
		 * PARENT — fork failed
		 */
		printlogf(LOG_ERR, "unable to fork (user %s %s)\n",
				line->cl_UserName,
				line->cl_Description);
		line->cl_Pid = 0;
		if (mailFd >= 0) {
			close(mailFd);
			remove(mailFile);
		}
		return;

	} else {
		/*
		 * PARENT — fork succeeded; rename mail file to include the child's PID
		 */
		char mailFile2[SMALL_BUFFER];

		make_mailfile(mailFile2, sizeof(mailFile2), file->cf_UserName, (int)line->cl_Pid);
		if (rename(mailFile, mailFile2) < 0) {
			printlogf(LOG_WARNING, "rename of mail file failed: %s\n", strerror(errno));
			remove(mailFile);
			line->cl_MailFlag = 0;
		}
	}

	if (mailFd >= 0)
		close(mailFd);
}

/*
 * EndJob — called when the main cron job process terminates
 */

void
EndJob(CronFile *file, CronLine *line, int exit_status)
{
	int mailFd;
	char mailFile[SMALL_BUFFER];
	struct stat sbuf;
	struct CronNotifier *notif;

	if (line->cl_Pid <= 0) {
		/* No job running — should never happen */
		line->cl_Pid = 0;
		return;
	}

	/*
	 * Handle frequency-based (timestamped) jobs
	 */
	if (line->cl_Delay > 0) {
		if (exit_status == EAGAIN) {
			/*
			 * Job returned EAGAIN: retry after cl_Delay.
			 * We base the next NotUntil on when the job was *scheduled*,
			 * not when it finished.
			 */
		} else {
			/*
			 * Job finished normally (possibly with an error code).
			 * Write the timestamp and advance NotUntil.
			 */
			FILE *fi;
			char buf[SMALL_BUFFER];
			int succeeded = 0;

			line->cl_LastRan = line->cl_NotUntil - line->cl_Delay;
			if ((fi = fopen(line->cl_Timestamp, "w")) != NULL) {
				struct tm *ltm = localtime(&line->cl_LastRan);
				if (ltm && strftime(buf, sizeof(buf), CRONSTAMP_FMT, ltm))
					if (fputs(buf, fi) >= 0)
						succeeded = 1;
				fclose(fi);
			}
			if (!succeeded)
				printlogf(LOG_WARNING,
						"unable to write timestamp to %s (user %s %s)\n",
						line->cl_Timestamp,
						file->cf_UserName,
						line->cl_Description);

			line->cl_NotUntil = line->cl_LastRan +
					((line->cl_Freq > 0) ? line->cl_Freq : line->cl_Delay);
		}
	}

	if (exit_status != EAGAIN) {
		/* notify any waiting jobs */
		for (notif = line->cl_Notifs; notif; notif = notif->cn_Next) {
			if (notif->cn_Waiter)
				notif->cn_Waiter->cw_Flag = exit_status;
		}

		if (exit_status) {
			printlogf(LOG_NOTICE, "exit status %d from user %s %s\n",
					exit_status,
					file->cf_UserName,
					line->cl_Description);
		}
	}

	if (DebugOpt && (!exit_status || exit_status == EAGAIN))
		printlogf(LOG_DEBUG, "exit status %d from user %s %s\n",
				exit_status,
				file->cf_UserName,
				line->cl_Description);

	if (line->cl_MailFlag != 1) {
		line->cl_Pid = 0;
		return;
	}

	/*
	 * Determine mail file path before clearing cl_Pid
	 */
	make_mailfile(mailFile, sizeof(mailFile), file->cf_UserName, (int)line->cl_Pid);
	line->cl_Pid = 0;
	line->cl_MailFlag = 0;

	/*
	 * Open and validate the mail file before deciding whether to send it.
	 */
	mailFd = open(mailFile, O_RDONLY|O_CLOEXEC);
	remove(mailFile);
	if (mailFd < 0)
		return;

	if (fstat(mailFd, &sbuf) < 0 ||
			sbuf.st_uid  != DaemonUid   ||
			sbuf.st_nlink != 0          ||
			sbuf.st_size == line->cl_MailPos ||
			!S_ISREG(sbuf.st_mode)) {
		close(mailFd);
		return;
	}

	/*
	 * Fork to send the mail as the target user
	 */
	line->cl_Pid = fork();

	if (line->cl_Pid == 0) {
		/*
		 * CHILD — drop privileges and send mail
		 */

		if (ChangeUser(file->cf_UserName, TempDir) < 0) {
			printlogf(LOG_ERR, "unable to ChangeUser to send mail (user %s %s)\n",
					file->cf_UserName,
					line->cl_Description);
			_exit(1);
		}

		dup2(2, 8);
		fcntl(8, F_SETFD, FD_CLOEXEC);
		fclose(stderr);

		/* stdin = mail file, stderr = /dev/null */
		dup2(mailFd, 0);
		dup2(1, 2);
		close(mailFd);

		if (SendScript != NULL) {
			/*
			 * Custom send script: pass the crontab filename and job name.
			 * Use a copy of the description so basename() doesn't mutate it.
			 */
			char descbuf[SMALL_BUFFER];
			snprintf(descbuf, sizeof(descbuf), "%s", line->cl_Description);

			fdprintlogf(LOG_DEBUG, 8, "writing using custom %s\n", SendScript);
			execl(SendScript, SendScript, file->cf_FileName, basename(descbuf), NULL);

		} else if (SendMail != NULL) {
			/*
			 * Custom mailer binary specified via -M
			 */
			execl(SendMail, SendMail, NULL);

		} else {
			/*
			 * Standard sendmail path
			 */
			fdprintlogf(LOG_INFO, 8, "mailing cron output for user %s %s\n",
					file->cf_UserName,
					line->cl_Description);
			execl(SENDMAIL, SENDMAIL, SENDMAIL_ARGS, NULL);

			/* exec failed — fall through to error log below */
			SendMail = SENDMAIL;
		}

		fdprintlogf(LOG_WARNING, 8,
				"unable to exec %s: cron output for user %s %s to /dev/null\n",
				SendMail ? SendMail : SENDMAIL,
				file->cf_UserName,
				line->cl_Description);
		_exit(1);

	} else if (line->cl_Pid < 0) {
		/*
		 * PARENT — fork failed
		 */
		printlogf(LOG_WARNING, "unable to fork: cron output for user %s %s to /dev/null\n",
				file->cf_UserName,
				line->cl_Description);
		line->cl_Pid = 0;
	} else {
		/*
		 * PARENT — fork succeeded.  The mailjob is caught by the SIGCHLD handler.
		 */
		line->cl_Pid = 0;
	}

	close(mailFd);
}
