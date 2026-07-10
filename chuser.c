/*
 * CHUSER.C
 *
 * Copyright 1994 Matthew Dillon (dillon@apollo.backplane.com)
 * Copyright 2009-2019 James Pryor <dubiousjim@gmail.com>
 * May be distributed under the GNU General Public License version 2 or any later version.
 */

#include "defs.h"

Prototype int ChangeUser(const char *user, const char *dochdir);

int
ChangeUser(const char *user, const char *dochdir)
{
	struct passwd *pas;

	/*
	 * Obtain password entry and change privileges
	 */

	if ((pas = getpwnam(user)) == NULL) {
		printlogf(LOG_ERR, "failed to get uid for %s\n", user);
		return -1;
	}

	/* Set environment for the user; ignore individual failures gracefully */
	(void)setenv("USER",    pas->pw_name, 1);
	(void)setenv("LOGNAME", pas->pw_name, 1);
	(void)setenv("HOME",    pas->pw_dir,  1);
	(void)setenv("SHELL",   "/bin/sh",    1);

	/*
	 * Change running state to the user in question.
	 * Use setres{g,u}id so the drop is irreversible (saved-set-ID also cleared).
	 */

	if (initgroups(user, pas->pw_gid) < 0) {
		printlogf(LOG_ERR, "initgroups failed: %s %s\n", user, strerror(errno));
		return -1;
	}
	if (setresgid(pas->pw_gid, pas->pw_gid, pas->pw_gid) < 0) {
		printlogf(LOG_ERR, "setresgid failed: %s gid=%d\n", user, (int)pas->pw_gid);
		return -1;
	}
	if (setresuid(pas->pw_uid, pas->pw_uid, pas->pw_uid) < 0) {
		printlogf(LOG_ERR, "setresuid failed: %s uid=%d\n", user, (int)pas->pw_uid);
		return -1;
	}
	if (dochdir) {
		/* try to change to $HOME */
		if (chdir(pas->pw_dir) < 0) {
			printlogf(LOG_ERR, "chdir failed: %s %s\n", user, pas->pw_dir);
			/* dochdir is a fallback directory, usually /tmp */
			if (chdir(dochdir) < 0) {
				printlogf(LOG_ERR, "chdir failed: %s %s\n", user, dochdir);
				return -1;
			}
		}
	}
	return (int)pas->pw_uid;
}
