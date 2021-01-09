

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/utsname.h>
#include <langinfo.h>
#include <utmpx.h>


#include "eloop.h"
#include "pty.h"
#include "shl_log.h"
#include "shl_misc.h"

#define LOG_SUBSYSTEM "issue"

#include "issue.h"



struct options {
	/* internal buffer, dont' worry */
	char	*osrelease;

	/* int		flags; */

	/* alternative issue file or dirname */
	const char	*issue;
	/* tty name */
	const char	*tty;
	const char	*seat;
};

static inline const char * _(const char *s) { return s; }

#ifdef TEST
static void log_err(const char *);
static void log_warn(const char *);
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#define _PATH_SYSCONFSTATICDIR	"/usr/lib"
#define _PATH_RUNSTATEDIR		"/run"
#define ISSUEDIR_EXT	".issue"
#define ISSUEDIR_EXTSIZ	(sizeof(ISSUEDIR_EXT) - 1)
#define _PATH_OS_RELEASE_ETC	"/etc/os-release"
#define _PATH_OS_RELEASE_USR	"/usr/lib/os-release"
#define _PATH_ISSUE_FILENAME	"issue"
#define _PATH_ISSUE_DIRNAME	_PATH_ISSUE_FILENAME ".d"
#define _PATH_ISSUE		"/etc/" _PATH_ISSUE_FILENAME
#define _PATH_ISSUEDIR		"/etc/" _PATH_ISSUE_DIRNAME

static inline int write_all(int fd, const void *buf, size_t count)
{
	while (count) {
		ssize_t tmp;

		errno = 0;
		tmp = write(fd, buf, count);
		if (tmp > 0) {
			count -= tmp;
			if (count)
				buf = (const void *) ((const char *) buf + tmp);
		} else if (errno != EINTR && errno != EAGAIN)
			return -1;
		if (errno == EAGAIN)	/* Try later, *sigh* */
			usleep(250000);
	}
	return 0;
}

static inline ssize_t read_all(int fd, char *buf, size_t count)
{
	ssize_t ret;
	ssize_t c = 0;
	int tries = 0;

	memset(buf, 0, count);
	while (count > 0) {
		ret = read(fd, buf, count);
		if (ret < 0) {
			if ((errno == EAGAIN || errno == EINTR) && (tries++ < 5)) {
				usleep(250000);
				continue;
			}
			return c ? c : -1;
		}
		if (ret == 0)
			return c;
		tries = 0;
		count -= ret;
		buf += ret;
		c += ret;
	}
	return c;
}

struct issue {
	FILE *output;
	char *mem;
	size_t mem_sz;

#ifdef AGETTY_RELOAD
	char *mem_old;
#endif
	unsigned int do_tcsetattr : 1,
		     do_tcrestore : 1;
};

static char *read_os_release(struct options *op, const char *varname)
{
	int fd = -1;
	struct stat st;
	size_t varsz = strlen(varname);
	char *p, *buf = NULL, *ret = NULL;

	/* read the file only once */
	if (!op->osrelease) {
		fd = open(_PATH_OS_RELEASE_ETC, O_RDONLY);
		if (fd == -1) {
			fd = open(_PATH_OS_RELEASE_USR, O_RDONLY);
			if (fd == -1) {
				log_warn(_("cannot open os-release file"));
				return NULL;
			}
		}

		if (fstat(fd, &st) < 0 || st.st_size > 4 * 1024 * 1024)
			goto done;

		op->osrelease = malloc(st.st_size + 1);
		if (!op->osrelease)
			log_err(_("failed to allocate memory: %m"));
		if (read_all(fd, op->osrelease, st.st_size) != (ssize_t) st.st_size) {
			free(op->osrelease);
			op->osrelease = NULL;
			goto done;
		}
		op->osrelease[st.st_size] = 0;
	}

	buf = strdup(op->osrelease);
	if (!buf)
		log_err(_("failed to allocate memory: %m"));
	p = buf;

	for (;;) {
		char *eol, *eon;

		p += strspn(p, "\n\r");
		p += strspn(p, " \t\n\r");
		if (!*p)
			break;
		if (strspn(p, "#;\n") != 0) {
			p += strcspn(p, "\n\r");
			continue;
		}
		if (strncmp(p, varname, varsz) != 0) {
			p += strcspn(p, "\n\r");
			continue;
		}
		p += varsz;
		p += strspn(p, " \t\n\r");

		if (*p != '=')
			continue;

		p += strspn(p, " \t\n\r=\"");
		eol = p + strcspn(p, "\n\r");
		*eol = '\0';
		eon = eol-1;
		while (eon > p) {
			if (*eon == '\t' || *eon == ' ') {
				eon--;
				continue;
			}
			if (*eon == '"') {
				*eon = '\0';
				break;
			}
			break;
		}
		free(ret);
		ret = strdup(p);
		if (!ret)
			log_err(_("failed to allocate memory: %m"));
		p = eol + 1;
	}
done:
	free(buf);
	if (fd >= 0)
		close(fd);
	return ret;
}


static int issuedir_filter(const struct dirent *d)
{
	size_t namesz;

#ifdef _DIRENT_HAVE_D_TYPE
	if (d->d_type != DT_UNKNOWN && d->d_type != DT_REG &&
	    d->d_type != DT_LNK)
		return 0;
#endif
	if (*d->d_name == '.')
		return 0;

	namesz = strlen(d->d_name);
	if (!namesz || namesz < ISSUEDIR_EXTSIZ + 1 ||
	    strcmp(d->d_name + (namesz - ISSUEDIR_EXTSIZ), ISSUEDIR_EXT) != 0)
		return 0;

	/* Accept this */
	return 1;
}



#if 0
static void print_issue_file(struct issue *ie,
			     struct options *op)
{
/*
 * 	TBD: what we do ?

	int oflag = tp->c_oflag;	    / * Save current setting. * /

	if ((op->flags & F_NONL) == 0) {
		/ * Issue not in use, start with a new line. * /
		write_all(STDOUT_FILENO, "\r\n", 2);
	}

	if (ie->do_tcsetattr) {
		if ((op->flags & F_VCONSOLE) == 0) {
			/ * Map new line in output to carriage return & new line. * /
			tp->c_oflag |= (ONLCR | OPOST);
			tcsetattr(STDIN_FILENO, TCSADRAIN, tp);
		}
	}
*/
	if (ie->mem_sz)
		write_all(STDOUT_FILENO, ie->mem, ie->mem_sz);
/*
	if (ie->do_tcrestore) {
		/ * Restore settings. * /
		tp->c_oflag = oflag;
		/ * Wait till output is gone. * /
		tcsetattr(STDIN_FILENO, TCSADRAIN, tp);
	}
*/
#ifdef AGETTY_RELOAD
	free(ie->mem_old);
	ie->mem_old = ie->mem;
	ie->mem = NULL;
	ie->mem_sz = 0;
#else
	free(ie->mem);
	ie->mem = NULL;
	ie->mem_sz = 0;
#endif
}
#endif

static void print_addr(struct issue *ie, sa_family_t family, void *addr)
{
	char buff[INET6_ADDRSTRLEN + 1];

	inet_ntop(family, addr, buff, sizeof(buff));
	fprintf(ie->output, "%s", buff);
}


/*
 * MAXHOSTNAMELEN replacement
 */
static inline size_t get_hostname_max(void)
{
	long len = sysconf(_SC_HOST_NAME_MAX);

	if (0 < len)
		return len;

#ifdef MAXHOSTNAMELEN
	return MAXHOSTNAMELEN;
#elif HOST_NAME_MAX
	return HOST_NAME_MAX;
#endif
	return 64;
}

static char *xgetdomainname(void)
{

	char *name;
	const size_t sz = get_hostname_max() + 1;

	name = malloc(sizeof(char) * sz);
	if (!name)
		log_err(_("failed to allocate memory: %m"));

	if (getdomainname(name, sz) != 0) {
		free(name);
		return NULL;
	}
	name[sz - 1] = '\0';
	return name;
}

static char *xgethostname(void)
{
	char *name;
	size_t sz = get_hostname_max() + 1;

	name = malloc(sizeof(char) * sz);
	if (!name)
		log_err(_("failed to allocate memory: %m"));

	if (gethostname(name, sz) != 0) {
		free(name);
		return NULL;
	}
	name[sz - 1] = '\0';
	return name;
}


/*
 * Prints IP for the specified interface (@iface), if the interface is not
 * specified then prints the "best" one (UP, RUNNING, non-LOOPBACK). If not
 * found the "best" interface then prints at least host IP.
 */
static void output_iface_ip(struct issue *ie,
			    struct ifaddrs *addrs,
			    const char *iface,
			    sa_family_t family)
{
	struct ifaddrs *p;
	struct addrinfo hints, *info = NULL;
	char *host = NULL;
	void *addr = NULL;

	if (!addrs)
		return;

	for (p = addrs; p; p = p->ifa_next) {

		if (!p->ifa_name ||
		    !p->ifa_addr ||
		    p->ifa_addr->sa_family != family)
			continue;

		if (iface) {
			/* Filter out by interface name */
		       if (strcmp(p->ifa_name, iface) != 0)
				continue;
		} else {
			/* Select the "best" interface */
			if ((p->ifa_flags & IFF_LOOPBACK) ||
			    !(p->ifa_flags & IFF_UP) ||
			    !(p->ifa_flags & IFF_RUNNING))
				continue;
		}

		addr = NULL;
		switch (p->ifa_addr->sa_family) {
		case AF_INET:
			addr = &((struct sockaddr_in *)	p->ifa_addr)->sin_addr;
			break;
		case AF_INET6:
			addr = &((struct sockaddr_in6 *) p->ifa_addr)->sin6_addr;
			break;
		}

		if (addr) {
			print_addr(ie, family, addr);
			return;
		}
	}

	if (iface)
		return;

	/* Hmm.. not found the best interface, print host IP at least */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	if (family == AF_INET6)
		hints.ai_flags = AI_V4MAPPED;

	host = xgethostname();
	if (host && getaddrinfo(host, NULL, &hints, &info) == 0 && info) {
		switch (info->ai_family) {
		case AF_INET:
			addr = &((struct sockaddr_in *) info->ai_addr)->sin_addr;
			break;
		case AF_INET6:
			addr = &((struct sockaddr_in6 *) info->ai_addr)->sin6_addr;
			break;
		}
		if (addr)
			print_addr(ie, family, addr);

		freeaddrinfo(info);
	}
	free(host);
}

/*
 * From util-linux lib/color-names.c, include/color-names.h
 */
#define UL_COLOR_RESET		"\033[0m"
#define UL_COLOR_BOLD		"\033[1m"
#define UL_COLOR_HALFBRIGHT	"\033[2m"
#define UL_COLOR_UNDERSCORE	"\033[4m"
#define UL_COLOR_BLINK		"\033[5m"
#define UL_COLOR_REVERSE	"\033[7m"

/* Standard colors */
#define UL_COLOR_BLACK		"\033[30m"
#define UL_COLOR_RED		"\033[31m"
#define UL_COLOR_GREEN		"\033[32m"
#define UL_COLOR_BROWN		"\033[33m"	/* well, brown */
#define UL_COLOR_BLUE		"\033[34m"
#define UL_COLOR_MAGENTA	"\033[35m"
#define UL_COLOR_CYAN		"\033[36m"
#define UL_COLOR_GRAY		"\033[37m"

/* Bold variants */
#define UL_COLOR_DARK_GRAY	"\033[1;30m"
#define UL_COLOR_BOLD_RED	"\033[1;31m"
#define UL_COLOR_BOLD_GREEN	"\033[1;32m"
#define UL_COLOR_BOLD_YELLOW	"\033[1;33m"
#define UL_COLOR_BOLD_BLUE	"\033[1;34m"
#define UL_COLOR_BOLD_MAGENTA	"\033[1;35m"
#define UL_COLOR_BOLD_CYAN	"\033[1;36m"

#define UL_COLOR_WHITE		"\033[1;37m"

/* maximal length of human readable name of ESC seq. */
#define UL_COLORNAME_MAXSZ	32

struct ul_color_name {
	const char *name;
	const char *seq;
};

/*
 * qsort/bsearch buddy
 */
static int cmp_color_name(const void *a0, const void *b0)
{
	const struct ul_color_name
		*a = (const struct ul_color_name *) a0,
		*b = (const struct ul_color_name *) b0;
	return strcmp(a->name, b->name);
}

/*
 * Maintains human readable color names
 */
static const char *color_sequence_from_colorname(const char *str)
{
	static const struct ul_color_name basic_schemes[] = {
		{ "black",	UL_COLOR_BLACK           },
		{ "blink",      UL_COLOR_BLINK           },
		{ "blue",	UL_COLOR_BLUE            },
		{ "bold",       UL_COLOR_BOLD		 },
		{ "brown",	UL_COLOR_BROWN           },
		{ "cyan",	UL_COLOR_CYAN            },
		{ "darkgray",	UL_COLOR_DARK_GRAY       },
		{ "gray",	UL_COLOR_GRAY            },
		{ "green",	UL_COLOR_GREEN           },
		{ "halfbright", UL_COLOR_HALFBRIGHT	 },
		{ "lightblue",	UL_COLOR_BOLD_BLUE       },
		{ "lightcyan",	UL_COLOR_BOLD_CYAN       },
		{ "lightgray,",	UL_COLOR_GRAY            },
		{ "lightgreen", UL_COLOR_BOLD_GREEN      },
		{ "lightmagenta", UL_COLOR_BOLD_MAGENTA  },
		{ "lightred",	UL_COLOR_BOLD_RED        },
		{ "magenta",	UL_COLOR_MAGENTA         },
		{ "red",	UL_COLOR_RED             },
		{ "reset",      UL_COLOR_RESET,          },
		{ "reverse",    UL_COLOR_REVERSE         },
		{ "yellow",	UL_COLOR_BOLD_YELLOW     },
		{ "white",      UL_COLOR_WHITE           }
	};
	struct ul_color_name key = { .name = str }, *res;

	if (!str)
		return NULL;

	res = bsearch(&key, basic_schemes, ARRAY_SIZE(basic_schemes),
				sizeof(struct ul_color_name),
				cmp_color_name);
	return res ? res->seq : NULL;
}

/*
 * parses \x{argument}, if not argument specified then returns NULL, the @fd
 * has to point to one char after the sequence (it means '{').
 */
static char *get_escape_argument(FILE *fd, char *buf, size_t bufsz)
{
	size_t i = 0;
	int c = fgetc(fd);

	if (c == EOF || (unsigned char) c != '{') {
		ungetc(c, fd);
		return NULL;
	}

	do {
		c = fgetc(fd);
		if (c == EOF)
			return NULL;
		if ((unsigned char) c != '}' && i < bufsz - 1)
			buf[i++] = (unsigned char) c;

	} while ((unsigned char) c != '}');

	buf[i] = '\0';
	return buf;
}

static void output_special_char(struct issue *ie,
				unsigned char c,
				struct options *op,
				FILE *fp)
{
	struct utsname uts;

	switch (c) {
	case 'e':
	{
		char escname[UL_COLORNAME_MAXSZ];

		if (get_escape_argument(fp, escname, sizeof(escname))) {
			const char *esc = color_sequence_from_colorname(escname);
			if (esc)
				fputs(esc, ie->output);
		} else
			fputs("\033", ie->output);
		break;
	}
	case 's':
		uname(&uts);
		fprintf(ie->output, "%s", uts.sysname);
		break;
	case 'n':
		uname(&uts);
		fprintf(ie->output, "%s", uts.nodename);
		break;
	case 'r':
		uname(&uts);
		fprintf(ie->output, "%s", uts.release);
		break;
	case 'v':
		uname(&uts);
		fprintf(ie->output, "%s", uts.version);
		break;
	case 'm':
		uname(&uts);
		fprintf(ie->output, "%s", uts.machine);
		break;
	case 'o':
	{
		char *dom = xgetdomainname();

		fputs(dom ? dom : "unknown_domain", ie->output);
		free(dom);
		break;
	}
	case 'O':
	{
		char *dom = NULL;
		char *host = xgethostname();
		struct addrinfo hints, *info = NULL;

		memset(&hints, 0, sizeof(hints));
		hints.ai_flags = AI_CANONNAME;

		if (host && getaddrinfo(host, NULL, &hints, &info) == 0 && info) {
			char *canon;

			if (info->ai_canonname &&
			    (canon = strchr(info->ai_canonname, '.')))
				dom = canon + 1;
		}
		fputs(dom ? dom : "unknown_domain", ie->output);
		if (info)
			freeaddrinfo(info);
		free(host);
		break;
	}
	case 'd':
	case 't':
	{
		time_t now;
		struct tm tm;

		time(&now);
		localtime_r(&now, &tm);

		if (c == 'd') /* ISO 8601 */
			fprintf(ie->output, "%s %s %d  %d",
				      nl_langinfo(ABDAY_1 + tm.tm_wday),
				      nl_langinfo(ABMON_1 + tm.tm_mon),
				      tm.tm_mday,
				      tm.tm_year < 70 ? tm.tm_year + 2000 :
				      tm.tm_year + 1900);
		else
			fprintf(ie->output, "%02d:%02d:%02d",
				      tm.tm_hour, tm.tm_min, tm.tm_sec);
		break;
	}
	case 'l':
		fprintf (ie->output, "%s", op->tty);
		break;
	case 'b':
	{
		/*
		 * dropped from util-linux, in kmcon the baudrate doesn't make sense
		 */
		fprintf(ie->output, "<UNSUPPORTED>");
		break;
	}
	case 'S':
	{
		char *var = NULL, varname[64];

		/* \S{varname} */
		if (get_escape_argument(fp, varname, sizeof(varname))) {
			var = read_os_release(op, varname);
			if (var) {
				if (strcmp(varname, "ANSI_COLOR") == 0)
					fprintf(ie->output, "\033[%sm", var);
				else
					fputs(var, ie->output);
			}
		/* \S */
		} else if ((var = read_os_release(op, "PRETTY_NAME"))) {
			fputs(var, ie->output);

		/* \S and PRETTY_NAME not found */
		} else {
			uname(&uts);
			fputs(uts.sysname, ie->output);
		}

		free(var);

		break;
	}
	case 'u':
	case 'U':
	{
		int users = 0;
		struct utmpx *ut;
		setutxent();
		while ((ut = getutxent()))
			if (ut->ut_type == USER_PROCESS)
				users++;
		endutxent();
		if (c == 'U')
			if (users > 1)
				fprintf(ie->output, "%d users", users);
			else
				fprintf(ie->output, "%d user", users);
		else
			fprintf (ie->output, "%d ", users);
		break;
	}

	case 'T':
	{
		fprintf(ie->output, "%s", op->seat);
	}

	case '4':
	case '6':
	{
		sa_family_t family = c == '4' ? AF_INET : AF_INET6;
		struct ifaddrs *addrs = NULL;
		char iface[128];

		if (getifaddrs(&addrs))
			break;

		if (get_escape_argument(fp, iface, sizeof(iface)))
			output_iface_ip(ie, addrs, iface, family);
		else
			output_iface_ip(ie, addrs, NULL, family);

		freeifaddrs(addrs);

		break;
	}

	default:
		putc(c, ie->output);
		break;
	}
}


static int issuefile_read_stream(
		struct issue *ie, FILE *f,
		struct options *op)
{
	struct stat st;
	int c;

	if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode))
		return 1;

	if (!ie->output) {
		free(ie->mem);
		ie->mem_sz = 0;
		ie->mem = NULL;
		ie->output = open_memstream(&ie->mem, &ie->mem_sz);
	}

	while ((c = getc(f)) != EOF) {
		if (c == '\\')
			output_special_char(ie, getc(f), op, f);
		else
			putc(c, ie->output);
	}

	return 0;
}

static int issuefile_read(
		struct issue *ie, const char *filename,
		struct options *op)
{
	FILE *f = fopen(filename, "re");
	int rc = 1;

	if (f) {
		rc = issuefile_read_stream(ie, f, op);
		fclose(f);
	}
	return rc;
}

static inline FILE *fopen_at(int dir, const char *filename,
                             int flags, const char *mode)
{
	int fd = openat(dir, filename, flags);
	if (fd < 0)
		return NULL;

	return fdopen(fd, mode);
}

/* returns: 0 on success, 1 cannot open, <0 on error
 */
static int issuedir_read(struct issue *ie, const char *dirname,
			 struct options *op)
{
        int dd, nfiles, i;
        struct dirent **namelist = NULL;

	dd = open(dirname, O_RDONLY|O_CLOEXEC|O_DIRECTORY);
	if (dd < 0)
		return 1;

	nfiles = scandirat(dd, ".", &namelist, issuedir_filter, versionsort);
	if (nfiles <= 0)
		goto done;

	ie->do_tcsetattr = 1;

	for (i = 0; i < nfiles; i++) {
		struct dirent *d = namelist[i];
		FILE *f;

		f = fopen_at(dd, d->d_name, O_RDONLY|O_CLOEXEC, "re");
		if (f) {
			issuefile_read_stream(ie, f, op);
			fclose(f);
		}
	}

	for (i = 0; i < nfiles; i++)
		free(namelist[i]);
	free(namelist);
done:
	close(dd);
	return 0;
}

static void eval_issue_file(struct issue *ie,
			    struct options *op)
{
	int has_file = 0;

	/*
	 * The custom issue file or directory list specified by:
	 *   agetty --isue-file <path[:path]...>
	 * Note that nothing is printed if the file/dir does not exist.
	 */
	if (op->issue) {
		char *list = strdup(op->issue);
		char *file;

		if (!list)
			log_err(_("failed to allocate memory: %m"));

		for (file = strtok(list, ":"); file; file = strtok(NULL, ":")) {
			struct stat st;

			if (stat(file, &st) < 0)
				continue;
			if (S_ISDIR(st.st_mode))
				issuedir_read(ie, file, op);
			else
				issuefile_read(ie, file, op);
		}
		free(list);
		goto done;
	}

	/* The default /etc/issue and optional /etc/issue.d directory as
	 * extension to the file. The /etc/issue.d directory is ignored if
	 * there is no /etc/issue file. The file may be empty or symlink.
	 */
	if (access(_PATH_ISSUE, F_OK|R_OK) == 0) {
		issuefile_read(ie, _PATH_ISSUE, op);
		issuedir_read(ie, _PATH_ISSUEDIR, op);
		goto done;
	}

	/* Fallback @runstatedir (usually /run) -- the file is not required to
	 * read the dir.
	 */
	if (issuefile_read(ie, _PATH_RUNSTATEDIR "/" _PATH_ISSUE_FILENAME, op) == 0)
		has_file++;
	if (issuedir_read(ie, _PATH_RUNSTATEDIR "/" _PATH_ISSUE_DIRNAME, op) == 0)
		has_file++;
	if (has_file)
		goto done;

	/* Fallback @sysconfstaticdir (usually /usr/lib) -- the file is not
	 * required to read the dir
	 */
	issuefile_read(ie, _PATH_SYSCONFSTATICDIR "/" _PATH_ISSUE_FILENAME, op);
	issuedir_read(ie, _PATH_SYSCONFSTATICDIR "/" _PATH_ISSUE_DIRNAME, op);

done:

	if (ie->output) {
		fclose(ie->output);
		ie->output = NULL;
	}
}


/* This is --show-issue backend, executed by normal user on the current
 * terminal.
 */
void _show_issue(struct options *op)
{
	struct issue ie = { .output = NULL };

	/*
	 * TODO: remove me
	struct termios tp;
	memset(&tp, 0, sizeof(struct termios));
	if (tcgetattr(STDIN_FILENO, &tp) < 0)
		err(EXIT_FAILURE, _("failed to get terminal attributes: %m"));
	*/

	eval_issue_file(&ie, op);

	if (ie.mem_sz)
		write_all(STDOUT_FILENO, ie.mem, ie.mem_sz);
	if (ie.output)
		fclose(ie.output);
	free(ie.mem);
}

void show_issue(const char *issue, const char *tty, const char *seat) {
	struct options op = {
		.tty = tty,
		.seat = seat,
		.issue = issue,
	};
	_show_issue(&op);
	free(op.osrelease);
}
#ifdef TEST

void log_err(const char *err) {
	fprintf(stderr, "ERROR: %s\n", err);
}
void log_warn(const char *err) {
	fprintf(stderr, "WARNING: %s\n", err);
}

int main() {
	struct options op = {
		.issue = NULL,
		.tty = "ttyBoh",
	};

	show_issue(&op);
}
#endif
