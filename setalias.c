/*
 * This file is part of setalias
 * Copyright © M. Kristall
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of version 2 of the GNU General Public License as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of version 2 of the GNU General Public
 * License with this program; if not, write to the Free Software Foundation, Inc
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stddef.h>
#include <limits.h>
#include <stdarg.h>
#include <arpa/inet.h>
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <pwd.h>

#ifndef ALIASFILE
#define ALIASFILE "/etc/aliases"
#endif
#ifndef NEWALIASES
#define NEWALIASES "/usr/bin/newaliases"
#endif
#define NEWSUFFIX ".new"
#define OLDSUFFIX ".bak"
#define SUPERUID 0

const char *progname;

char *skipspaces (char *s) {
	while (*s && isspace (*s))
		s++;
	return s;
}

char *strnchr (char *s, int c, size_t l) {
	for (; *s && l > 0; l--, s++) {
		if (*s == c)
			return s;
	}
	return NULL;
}

char *copyquoted (char *in, char *out, size_t m) {
	char *p;
	size_t l;
	if (*in != '"')
		return NULL;

	in++;
	// the quote must terminate
	if (!(p = strnchr (in, '"', m)))
		return NULL;

	// length of output string
	l = p - in;
	// advance past "
	p++;
	// m must be bigger than l so there is space for the \0
	if (m <= l)
		return NULL;

	memcpy (out, in, l);
	out[l] = '\0';

	return p;
}

char *gettoken (char *in, char *out, size_t len) {
	in = skipspaces (in);

	if (!*in)
		return NULL;

	if (*in == '"')
		return copyquoted (in, out, len);

	while (*in) {
		if (isspace (*in))
			break;

		switch (*in) {
			case '\n':
			case '#':
			case ':':
				goto done;

			case '"':
				return NULL;

			default:
				// need space for this character and \0
				if (len < 2)
					return NULL;

				*out++ = *in++;
				len--;
		}
	}
	done:
	*out = '\0';

	return in;
}

int puttoken (char *out, const char *in, size_t m) {
	size_t chrs;
	int quote = 0;
	if (strpbrk (in, "\t\n :#"))
		quote = 1;

	chrs = strlen (in);
	// account for 0 or 2 quote characters and a \0
	if (m < chrs + quote * 2 + 1)
		return 0;

	if (quote)
		*out++ = '"';

	// does not copy the \0
	memcpy (out, in, chrs);
	out += chrs;

	if (quote)
		*out++ = '"';

	*out = '\0';
	return 1;
}

// name: alias
int getalias (char *in, char *user, char *alias, size_t m) {
	*user = *alias = '\0';
	in = skipspaces (in);

	// comment or end of line
	if (!*in || *in == '\n' || *in == '#')
		return 0;

	// name:
	if (!(in = gettoken (in, user, m)))
		return 0;

	in = skipspaces (in);
	if (*in != ':')
		return 0;

	in++;

	// alias
	if (!(in = gettoken (in, alias, m)))
		return 0;

	in = skipspaces (in);
	// comment or end of line
	if (*in && *in != '#' && *in != '\n')
		return 0;

	return 1;
}


int setalias (char *line, const char *alias, size_t n) {
	char *p = strchr (line, ':');
	if (!p)
		return 0;
	p++;

	if (!(p = skipspaces (p)))
		return 0;

	// space remaining
	n -= p - line;

	if (!puttoken (p, alias, n))
		return 0;
	p += strlen (p) + 1;

	if (n < p - line + 2)
		return 0;
	*p++ = '\n';
	*p = '\0';

	return 1;
}

int makealias (char *line, const char *user, const char *alias, size_t n) {
	size_t len;

	if (!puttoken (line, user, n))
		return 0;

	len = strlen (line);

	if (n < len + 3)
		return 0;
	line[len] = ':';
	line[len + 1] = '\t';

	if (!puttoken (line + len + 2, alias, n - len - 2))
		return 0;

	return 1;
}

struct optss {
	int delete, get, verbose;
	char *user, *file, *suffix;
};

void vprint (const struct optss *o, int l, const char *fmt, ...) {
	va_list ap;
	if (l > o->verbose)
		return;
	va_start (ap, fmt);
	vfprintf (stderr, fmt, ap);
	va_end (ap);
}

enum {
	TYPEFREE,
	TYPEBOOL,
	TYPEINC,
	TYPEINT,
	TYPESTR
};

struct optt {
	int type;
	size_t ofs;
	int root;
};

int aliases (const struct optss *o, const char *newalias) {
	FILE *in, *out = NULL;
	int ifd, ofd = -1;
	int lockret;
	struct stat st;
	size_t n = 0, l = strlen (o->file);
	ssize_t r;
	char new[strlen (NEWSUFFIX) + l + 1], old[strlen (OLDSUFFIX) + l + 1];
	char *line = NULL;
	int ok = 0;
	if (!(in = fopen (o->file, "r"))) {
		fprintf (stderr, "%s: could not aliases file: %s\n",
			progname, strerror (errno));
		return 0;
	}
	ifd = fileno (in);
#define RETRY(r, e) do { r = (e); } while (r == -1 && errno == EINTR)
	RETRY (lockret, flock (ifd, LOCK_SH));
	if (lockret == -1) {
		fprintf (stderr, "%s: could not lock %s: %s\n",
			progname, o->file, strerror (errno));
		goto fail;
	}

	if (!o->get) {
		RETRY (lockret, flock (ifd, LOCK_EX));
		if (lockret == -1) {
			fprintf (stderr, "%s: could not lock %s: %s\n",
				progname, o->file, strerror (errno));
			goto fail;
		}
		if (fstat (ifd, &st) != 0) {
			fprintf (stderr, "%s: could not stat %s: %s\n",
				progname, o->file, strerror (errno));
			goto fail;
		}
		memcpy (new, o->file, l);
		strcpy (new + l, NEWSUFFIX);
		if (!(out = fopen (new, "w"))) {
			fprintf (stderr, "%s: could not open %s: %s\n",
				progname, new, strerror (errno));
			goto fail;
		}
		ofd = fileno (out);
		RETRY (lockret, flock (ofd, LOCK_EX));
		if (lockret == -1) {
			fprintf (stderr, "%s: could not lock temp file: %s\n",
				progname, strerror (errno));
			goto fail;
		}
	}
	while ((r = getline (&line, &n, in)) != -1) {
		char user[1000], alias[1000];
		if (r == 0)
			continue;
		if (!getalias (line, user, alias, 1000))
			;
		else if (strcmp (user, o->user) == 0) {
			ok = 1;
			if (o->get) {
				printf ("%s's alias is %s\n",
					o->user, alias);
				break;
			}
			if (o->delete) {
				printf ("removed %s's alias (%s)\n",
					o->user, alias);
				continue;
			}
			if (strcmp (alias, newalias) == 0) {
				printf ("no change\n");
				goto fail;
			}
			if (!setalias (line, newalias, n)) {
				fprintf (stderr, "%s: could not set alias\n",
					progname);
				goto fail;
			}
			printf ("set %s's alias to %s (from %s)\n",
				o->user, newalias, alias);
			r = strlen (line);
		}
		if (out && !fwrite (line, r, 1, out)) {
			fprintf (stderr, "%s: updating failed: %s\n",
				progname, strerror (errno));
			goto fail;
		}
		/* no trailing \n */
		if (line[r - 1] != '\n')
			fwrite ("\n", 1, 1, out);
	}
	if (!ok) {
		if (o->get)
			printf ("%s has no alias\n", o->user);
		else if (o->delete) {
			fprintf (stderr, "%s: no change\n", progname);
			goto fail;
		} else if (!makealias (line, o->user, newalias, 1000)) {
			fprintf (stderr, "%s: updating failed\n",
				progname);
			goto fail;
		} else {
			fprintf (out, "%s\n", line);
			printf ("set %s's alias to %s\n", o->user, newalias);
		}
	}
	free (line);
	n = 0;
	flock (ifd, LOCK_UN);
	fclose (in);
	in = NULL;
	if (out) {
		if (fchmod (ofd, st.st_mode) != 0) {
			fprintf (stderr, "%s: could not chmod: %s\n",
				progname, strerror (errno));
			goto fail;
		}
		flock (ofd, LOCK_UN);
		fclose (out);
		out = NULL;
		memcpy (old, o->file, l);
		strcpy (old + l, OLDSUFFIX);
		unlink (old);
		if (rename (o->file, old) != 0) {
			fprintf (stderr, "%s: could not move aliases file: %s\n",
				progname, strerror (errno));
			goto fail;
		}
		if (rename (new, o->file) != 0) {
			rename (old, o->file);
			fprintf (stderr, "%s: could not move new aliases file %s: %s\n",
				progname, new, strerror (errno));
		}
	}
	return 1;
	fail:
	if (n)
		free (line);
	if (in) {
		flock (ifd, LOCK_UN);
		fclose (in);
	}
	if (out) {
		flock (ofd, LOCK_UN);
		fclose (out);
		unlink (new);
	}
	return 0;
}

static struct optt args[CHAR_MAX];

void _setupargs (void) {
	/* -f path to alias file */
	args['f'].type = TYPESTR,
	args['f'].ofs = offsetof (struct optss, file),
	args['f'].root = 1;
	/* -S suffix */
	args['S'].type = TYPESTR,
	args['S'].ofs = offsetof (struct optss, suffix),
	args['S'].root = 1;
	/* -u user to set */
	args['u'].type = TYPESTR,
	args['u'].ofs = offsetof (struct optss, user),
	args['u'].root = 1;
	/* -d delete/unset alias */
	args['d'].type = TYPEBOOL,
	args['d'].ofs = offsetof (struct optss, delete);
	/* -v verbosity */
	args['v'].type = TYPEINC,
	args['v'].ofs = offsetof (struct optss, verbose);
}

/* handle args, returns number of arguments consumed */
int parg (struct optss *o, int argc, char **argv, int n, int root) {
	char *c;
	for (c = argv[n]; *c == '-'; n++) {
		void *p;
		int a;
		for (p = NULL, c++; (a = *c); p = NULL, c++) {
			if (a < 0 || !args[a].type) {
				fprintf (stderr,
					"%s: invalid argument '-%c'\n",
					progname, a);
				return -1;
			}
			if (args[a].root && !root) {
				fprintf (stderr,
					"%s: permission denied setting '-%c'\n",
					progname, a);
				return -1;
			}
			p = (char *)o + args[a].ofs;
			if (args[a].type != TYPEINC && *(char *)p) {
				fprintf (stderr,
					"%s: duplicate '-%c'\n",
					progname, a);
				return -1;
			}
			/* arguments expecting no value here */
			if (args[a].type == TYPEBOOL)
				*(int *)p = 1;
			else if (args[a].type == TYPEINC)
				(*(int *)p)++;
			else
				/* other arg types eat arguments */
				break;
		}
		if (!p)
			continue;
		/* we are expecting a value */
		/* we've reached the end of this arg so check the next */
		c++;
		if (!*c) {
			if (++n >= argc) {
				fprintf (stderr,
					"%s: argument expected for '-%c'\n",
					progname, a);
				return -1;
			}
			c = argv[n];
		}
		if (args[a].type == TYPEINT) {
			char *b;
			long v = strtol (c, &b, 10);
			if (*b) {
				fprintf (stderr,
					"%s: expected integer for '-%c', got '%s'\n",
					progname, a, c);
				return -1;
			}
			if (v < INT_MIN || v > INT_MAX) {
				fprintf (stderr,
					"%s: value for '-%c' out of range\n",
					progname, a);
				return -1;
			}
			*(int *)p = (int)v;
		} else if (args[a].type == TYPESTR)
			*(char **)p = c;
		else {
			fprintf (stderr,
				"%s: '-%c' is not fully implemented\n",
				progname, a);
			return -2;
		}
	}
	return n;
}

int validip (char *t) {
	struct in6_addr a;
	char *e;
	int af = AF_INET, r;
	if (*t++ != '[')
		return 0;
	if (!(e = strchr (t, ']')) || e[1])
		return 0;
	*e = '\0';
	if (strncmp (t, "IPv6:", 5) == 0) {
		t += 5;
		af = AF_INET6;
	}
	r = inet_pton (af, t, &a);
	*e = ']';
	return r;
}

#define UTFCONT if ((*(++(*t)) & 0xc0) != 0x80) return 0
int validutf8 (char **t) {
	switch (**t & 0xf0) {
		default: return 0;
		case 0xf0: UTFCONT;
		case 0xe0: UTFCONT;
		case 0xc0: UTFCONT;
		case 0x80: UTFCONT;
	}
	return 1;
}

int validalias (char *t) {
	char *s = t;
	int at = 0;
	/* this is a local user account */
	if (getpwnam (t))
		return 1;
	/* otherwise, allow an email address */
	for (; *t; t++) {
		while (validutf8 (&t))
			;
		if (!*t)
			break;
		if (isalnum (*t))
			;
		else if (*t == '.') {
			/* cannot be first or last */
			if (
				t == s ||
				t[-1] == '@' ||
				t[1] == '@' ||
				t[1] == '\0'
			)
				return 0;
		} else if (at) {
			/* - cannot be first or last in domain */
			if (*t == '-') {
				if (t[-1] == '@' || t[1] == '\0')
					return 0;
			} else
				return 0;
		} else if (*t == '@') {
			/* cannot be first or last */
			if (t == s || !t[1])
				return 0;
			at = 1;
			if (t[1] == '[')
				return validip (t + 1);
		} else {
			/* in username */
			switch (*t) {
				/* these are not all contiguous, so… */
				case '!': case '#': case '$': case '%':
				case '&': case '\'': case '*': case '+':
				case '/': case '=': case '?': case '^':
				case '_': case '`': case '{': case '|':
				case '}': case '~':
					break;
				default:
					return 0;
			}
		}
	}
	return at;
}

int main (int argc, char **argv) {
	struct optss opts = {0};
	uid_t uid =
#ifdef NDEBUG
		getuid ();
#else
		SUPERUID;
#endif
	char *alias = NULL;
	int i;
	progname = argv[0];
	_setupargs ();
	for (i = 1; i < argc; i++) {
		if (*argv[i] == '-') {
			i = parg (&opts, argc, argv, i, uid == SUPERUID);
			if (i < 0)
				return 1;
			--i;
		} else {
			if (alias) {
				fprintf (stderr,
					"%s: you can only specify one alias\n",
					progname);
				return 1;
			}
			alias = argv[i];
		}
	}
	/* now figure out how we are operating */
	if (!opts.file)
		opts.file = ALIASFILE;
	if (!opts.suffix)
		opts.suffix = OLDSUFFIX;
	/* try to figure out what user means */
	if (opts.user) {
		struct passwd *pw;
		/* is this a uid? */
		if (isdigit (*opts.user)) {
			uid = atoi (opts.user);
			if (!(pw = getpwuid (uid))) {
				endpwent ();
				fprintf (stderr,
					"%s: invalid uid '%d'\n",
					progname, uid);
				return 1;
			}
			opts.user = pw->pw_name;
		} else if (!(pw = getpwnam (opts.user))) {
			endpwent ();
			fprintf (stderr,
				"%s: invalid user '%s'\n",
				progname, opts.user);
			return 1;
		}
	} else {
		struct passwd *pw = getpwuid (uid);
		if (!pw) {
			endpwent ();
			fprintf (stderr,
				"%s: you have no passwd entry\n",
				progname);
			return 0;
		}
		opts.user = pw->pw_name;
	}
	if (alias) {
		if (strcmp (opts.user, alias) == 0)
			opts.delete = 1;
		else if (!validalias (alias)) {
			fprintf (stderr,
				"%s: '%s' does not look like a valid alias\n",
				progname, alias);
			return 1;
		}
	}
	opts.get = !alias && !opts.delete;
	vprint (&opts, 1,
		"%s configuration:\n"
		"  verbosity = %d\n"
		"  user      = %s\n"
		"  file      = %s\n"
		"  suffix    = %s\n"
		"  delete    = %s\n"
		"  get       = %s\n"
		"  alias     = %s\n",
		progname,
		opts.verbose, opts.user, opts.file, opts.suffix,
		opts.delete ? "true" : "false",
		opts.get ? "true" : "false",
		alias);
	if (NEWALIASES) {
		struct stat st;
		if (stat (NEWALIASES, &st) != 0) {
			fprintf (stderr,
				"%s: cannot stat %s\n",
				progname, NEWALIASES);
			return 2;
		}
		if (st.st_uid != SUPERUID) {
			fprintf (stderr,
				"%s: %s is not owned by root\n",
				progname, NEWALIASES);
			return 2;
		}
		if (st.st_mode & (S_IWGRP | S_IWOTH)) {
			fprintf (stderr,
				"%s: %s does not seem secure\n",
				progname, NEWALIASES);
			return 2;
		}
	}
	if (!aliases (&opts, alias)) {
		endpwent ();
		return 1;
	}
	endpwent ();
	if (NEWALIASES) {
		setuid (SUPERUID);
		execl (NEWALIASES, NEWALIASES, (char *)NULL);
	}
	return 0;
}
