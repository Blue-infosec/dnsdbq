/*
 * Copyright (c) 2014-2018 by Farsight Security, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* External. */

/* asprintf() does not appear on linux without this */
#define _GNU_SOURCE

/* gettimeofday() does not appear on linux without this. */
#define _BSD_SOURCE

/* modern glibc will complain about the above if it doesn't see this. */
#define _DEFAULT_SOURCE

#include <sys/wait.h>
#include <sys/time.h>
#include <sys/errno.h>

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <jansson.h>
#include "ns_ttl.h"

extern char **environ;

/* Types. */

#include "pdns.h"

struct reader {
	struct reader	*next;
	struct writer	*writer;
	CURL		*easy;
	struct curl_slist  *hdrs;
	char		*url;
	char		*buf;
	size_t		len;
	long		rcode;
};
typedef struct reader *reader_t;

struct writer {
	struct writer	*next;
	struct reader	*readers;
	u_long		after;
	u_long		before;
	FILE		*sort_stdin;
	FILE		*sort_stdout;
	pid_t		sort_pid;
	bool		sort_killed;
	int		count;
	char		*status;
	char		*message;
	bool		once;
};
typedef struct writer *writer_t;

struct verb {
	const char	*cmd_opt_val;
	const char	*url_fragment;
	/* validate_cmd_opts can review the command line options and exit
	 * if some verb-specific command line option constraint is not met.
	 */
	void		(*validate_cmd_opts)(void);
};
typedef const struct verb *verb_t;

struct pdns_sys {
	const char	*name;
	const char	*base_url;
	/* first argument is the input URL path.
	 * second is an output parameter pointing to
	 * the separator character (? or &) that the caller should
	 * use between any further URL parameters.  May be
	 * NULL if the caller doesn't care.
	 */
	char *		(*url)(const char *, char *);
	void		(*request_info)(void);
	void		(*write_info)(reader_t);
	void		(*auth)(reader_t);
	const char *	(*status)(reader_t);
	const char *	(*validate_verb)(const char *);
};
typedef const struct pdns_sys *pdns_sys_t;

typedef enum { no_mode = 0, rrset_mode, name_mode, ip_mode,
	       raw_rrset_mode, raw_name_mode } mode_e;

struct query {
	mode_e	mode;
	char	*thing;
	char	*rrtype;
	char	*bailiwick;
	char	*pfxlen;
	u_long	after;
	u_long	before;
};
typedef struct query *query_t;
typedef const struct query *query_ct;

struct sortbuf { char *base; size_t size; };
typedef struct sortbuf *sortbuf_t;

struct sortkey { char *specified, *computed; };
typedef struct sortkey *sortkey_t;
typedef const struct sortkey *sortkey_ct;

#include "pdns_dnsdb.h"
#include "pdns_circl.h"

/* Forward. */

static void help(void);
static bool parse_long(const char *, long *);
static void report_version(void);
static void debug(bool, const char *, ...);
static __attribute__((noreturn)) void usage(const char *, ...);
static __attribute__((noreturn)) void my_exit(int);
static __attribute__((noreturn)) void my_panic(bool, const char *);
static void server_setup(void);
static const char *add_sort_key(const char *);
static sortkey_ct find_sort_key(const char *);
static pdns_sys_t find_system(const char *);
static verb_t find_verb(const char *);
static void read_configs(void);
static void read_environ(void);
static void do_batch(FILE *, u_long, u_long);
static const char *batch_parse(char *, query_t);
static char *makepath(mode_e, const char *, const char *,
		      const char *, const char *);
static void make_curl(void);
static void unmake_curl(void);
static void pdns_query(query_ct);
static void query_launcher(query_ct, writer_t);
static void launch(const char *, writer_t, u_long, u_long, u_long, u_long);
static void reader_launch(writer_t, char *);
static void reader_reap(reader_t);
static void ruminate_json(int, u_long, u_long);
static writer_t writer_init(u_long, u_long);
static void writer_status(writer_t, const char *, const char *);
static size_t writer_func(char *ptr, size_t size, size_t nmemb, void *blob);
static int input_blob(const char *, size_t, u_long, u_long, FILE *);
static void writer_fini(writer_t);
stativ void unmake_writers(void);
static void io_engine(int);
static int timecmp(u_long, u_long);
static const char * time_str(u_long, bool);
static int time_get(const char *src, u_long *dst);
static void escape(char **);
static char *sortable_rrname(pdns_tuple_ct);
static char *sortable_rdata(pdns_tuple_ct);
static void sortable_rdatum(sortbuf_t, const char *, const char *);
static void sortable_dnsname(sortbuf_t, const char *);
static void sortable_hexify(sortbuf_t, const u_char *, size_t);
static void validate_cmd_opts_lookup(void);
static void validate_cmd_opts_summarize(void);
static const char *or_else(const char *, const char *);

#include "globals.h"

/* Constants. */

static const char * const conf_files[] = {
	"~/.isc-dnsdb-query.conf",
	"~/.dnsdb-query.conf",
	"/etc/isc-dnsdb-query.conf",
	"/etc/dnsdb-query.conf",
	NULL
};

static const char path_sort[] = "/usr/bin/sort";
static const char json_header[] = "Accept: application/json";
static const char env_time_fmt[] = "DNSDBQ_TIME_FORMAT";

static const struct pdns_sys pdns_systems[] = {
	/* note: element [0] of this array is the DEFAULT_SYS. */
	{ "dnsdb", "https://api.dnsdb.info",
	  dnsdb_url, dnsdb_request_info, dnsdb_write_info,
	  dnsdb_auth, dnsdb_status, dnsdb_validate_verb },
#if WANT_PDNS_CIRCL
	{ "circl", "https://www.circl.lu/pdns/query",
	  circl_url, NULL, NULL,
	  circl_auth, circl_status, circl_validate_verb },
#endif
	{ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static const struct verb verbs[] = {
	/* note: element [0] of this array is the DEFAULT_VERB. */
	{ "lookup", "/lookup", validate_cmd_opts_lookup },
	{ "summarize", "/summarize", validate_cmd_opts_summarize },
	{ NULL, NULL, NULL }
};

#define DEFAULT_SYS 0
#define DEFAULT_VERB 0
#define	MAX_KEYS 5
#define	MAX_JOBS 8

#define CREATE(p, s) if ((p) != NULL) { my_panic(false, "non-NULL ptr"); } \
	else if (((p) = malloc(s)) == NULL) { my_panic(true, "malloc"); } \
	else { memset((p), 0, s); }
#define DESTROY(p) { if ((p) != NULL) { free(p); (p) = NULL; } }
#define DEBUG(ge, ...) { if (debug_level >= (ge)) debug(__VA_ARGS__); }

/* Private. */

static const char *program_name = NULL;
static verb_t chosen_verb = &verbs[DEFAULT_VERB];
static pdns_sys_t sys = &pdns_systems[DEFAULT_SYS];
static enum { batch_none, batch_original, batch_verbose } batching = batch_none;
static bool merge = false;
static bool complete = false;
static bool info = false;
static bool gravel = false;
static bool donotverify = false;
static bool quiet = false;
static int debug_level = 0;
static enum { no_sort = 0, normal_sort, reverse_sort } sorted = no_sort;
static int curl_cleanup_needed = 0;
static present_t pres = present_text;
static long query_limit = -1;	/* -1 means not set on command line. */
static long output_limit = -1;	/* -1 means not set on command line. */
static long offset = 0;
static long max_count = 0;
static CURLM *multi = NULL;
static struct timeval now;
static int nkeys = 0;
static struct sortkey keys[MAX_KEYS];
static bool sort_byname = false;
static bool sort_bydata = false;
static writer_t writers = NULL;
static int exit_code = 0; /* hopeful */
static size_t ideal_buffer;
static bool iso8601 = false;

/* Public. */

int
main(int argc, char *argv[]) {
	mode_e mode = no_mode;
	char *thing = NULL, *rrtype = NULL, *bailiwick = NULL, *pfxlen = NULL;
	u_long after = 0;
	u_long before = 0;
	int json_fd = -1;
	char *value;
	int ch;

	/* global dynamic initialization. */
	ideal_buffer = 4 * (size_t) sysconf(_SC_PAGESIZE);
	gettimeofday(&now, NULL);
	if ((program_name = strrchr(argv[0], '/')) == NULL)
		program_name = argv[0];
	else
		program_name++;
	value = getenv(env_time_fmt);
	if (value != NULL && strcasecmp(value, "iso") == 0)
		iso8601 = true;

	/* process the command line options. */
	while ((ch = getopt(argc, argv,
			    "A:B:R:r:N:n:i:l:L:M:u:p:t:b:k:J:O:V:"
			    "cdfghIjmqSsUv"))

	       != -1)
	{
		switch (ch) {
		case 'A':
			if (!time_get(optarg, &after) || after == 0UL)
				usage("bad -A timestamp: '%s'\n", optarg);
			break;
		case 'B':
			if (!time_get(optarg, &before) || before == 0UL)
				usage("bad -B timestamp: '%s'\n", optarg);
			break;
		case 'R': {
			const char *p;

			if (mode != no_mode)
				usage("-r, -n, -i, -N, or -R "
				      "can only appear once");
			assert(thing == NULL);
			mode = raw_rrset_mode;

			p = strchr(optarg, '/');
			if (p != NULL) {
				if (rrtype != NULL || bailiwick != NULL)
					usage("if -b or -t are specified then "
					      "-R cannot contain a slash");

				const char *q;

				q = strchr(p + 1, '/');
				if (q != NULL) {
					bailiwick = strdup(q + 1);
					rrtype = strndup(p + 1,
							 (size_t)(q - p - 1));
				} else {
					rrtype = strdup(p + 1);
				}
				thing = strndup(optarg, (size_t)(p - optarg));
			} else {
				thing = strdup(optarg);
			}
			break;
		    }
		case 'r': {
			const char *p;

			if (mode != no_mode)
				usage("-r, -n, -i, -N, or -R "
				      "can only appear once");
			assert(thing == NULL);
			mode = rrset_mode;

			p = strchr(optarg, '/');
			if (p != NULL) {
				if (rrtype != NULL || bailiwick != NULL)
					usage("if -b or -t are specified then "
					      "-r cannot contain a slash");

				const char *q;

				q = strchr(p + 1, '/');
				if (q != NULL) {
					bailiwick = strdup(q + 1);
					rrtype = strndup(p + 1,
							 (size_t)(q - p - 1));
				} else {
					rrtype = strdup(p + 1);
				}
				thing = strndup(optarg, (size_t)(p - optarg));
			} else {
				thing = strdup(optarg);
			}
			break;
		    }
		case 'N': {
			const char *p;

			if (mode != no_mode)
				usage("-r, -n, -i, -N, or -R "
				      "can only appear once");
			assert(thing == NULL);
			mode = raw_name_mode;

			p = strchr(optarg, '/');
			if (p != NULL) {
				if (rrtype != NULL || bailiwick != NULL)
					usage("if -b or -t are specified then "
					      "-N cannot contain a slash");

				const char *q;

				q = strchr(p + 1, '/');
				if (q != NULL) {
					bailiwick = strdup(q + 1);
					rrtype = strndup(p + 1,
							 (size_t)(q - p - 1));
				} else {
					rrtype = strdup(p + 1);
				}
				thing = strndup(optarg, (size_t)(p - optarg));
			} else {
				thing = strdup(optarg);
			}
			break;
		    }
		case 'n': {
			const char *p;

			if (mode != no_mode)
				usage("-r, -n, -i, -N, or -R "
				      "can only appear once");
			assert(thing == NULL);
			mode = name_mode;

			p = strchr(optarg, '/');
			if (p != NULL) {
				if (rrtype != NULL || bailiwick != NULL)
					usage("if -b or -t are specified then "
					      "-n cannot contain a slash");

				const char *q;

				q = strchr(p + 1, '/');
				if (q != NULL) {
					bailiwick = strdup(q + 1);
					rrtype = strndup(p + 1,
							 (size_t)(q - p - 1));
				} else {
					rrtype = strdup(p + 1);
				}
				thing = strndup(optarg, (size_t)(p - optarg));
			} else {
				thing = strdup(optarg);
			}
			break;
		    }
		case 'i': {
			const char *p;

			if (mode != no_mode)
				usage("-r, -n, -i, -N, or -R "
				      "can only appear once");
			assert(thing == NULL);
			mode = ip_mode;
			p = strchr(optarg, '/');
			if (p != NULL) {
				thing = strndup(optarg, (size_t)(p - optarg));
				pfxlen = strdup(p + 1);
			} else {
				thing = strdup(optarg);
			}
			break;
		    }
		case 'V': {
			chosen_verb = find_verb(optarg);
			if (chosen_verb == NULL)
				usage("Unsupported verb for -V argument");
			break;
		    }
		case 'l':
			if (!parse_long(optarg, &query_limit) ||
			    (query_limit < 0))
				usage("-l must be zero or positive");
			break;
		case 'L':
			if (!parse_long(optarg, &output_limit) ||
			    (output_limit <= 0))
				usage("-L must be positive");
			break;
		case 'M':
			if (!parse_long(optarg, &max_count) || (max_count <= 0))
				usage("-M must be positive");
			break;
		case 'O':
			if (!parse_long(optarg, &offset) || (offset < 0))
				usage("-O must be zero or positive");
			break;
		case 'u':
			sys = find_system(optarg);
			if (sys == NULL)
				usage("-u must refer to a pdns system");
			break;
		case 'U':
			donotverify = true;
			break;
		case 'p':
			if (strcasecmp(optarg, "json") == 0)
				pres = present_json;
			else if (strcasecmp(optarg, "csv") == 0)
				pres = present_csv;
			else if (strcasecmp(optarg, "text") == 0 ||
				 strcasecmp(optarg, "dns") == 0)
			{
				pres = present_text;
			} else {
				usage("-p must specify json, text, or csv");
			}
			break;
		case 't':
			if (rrtype != NULL)
				usage("can only specify rrtype one way");
			rrtype = strdup(optarg);
			break;
		case 'b':
			if (bailiwick != NULL)
				usage("can only specify bailiwick one way");
			bailiwick = strdup(optarg);
			break;
		case 'k': {
			char *saveptr = NULL;
			const char *tok;

			if (sorted == no_sort)
				usage("-k must be preceded by -s or -S");
			for (tok = strtok_r(optarg, ",", &saveptr);
			     tok != NULL;
			     tok = strtok_r(NULL, ",", &saveptr))
			{
				const char *msg;

				if (find_sort_key(tok) != NULL)
					usage("Each sort key may only be "
					      "specified once");

				if ((msg = add_sort_key(tok)) != NULL)
					usage(msg);
			}
			break;
		    }
		case 'J':
			if (strcmp(optarg, "-") == 0)
				json_fd = STDIN_FILENO;
			else
				json_fd = open(optarg, O_RDONLY);
			if (json_fd < 0)
				my_panic(true, optarg);
			break;
		case 'd':
			debug_level++;
			break;
		case 'g':
			gravel = true;
			break;
		case 'j':
			pres = present_json;
			break;
		case 'f':
			switch (batching) {
			case batch_none:
				batching = batch_original;
				break;
			case batch_original:
				batching = batch_verbose;
				break;
			case batch_verbose:
				/* FALLTHROUGH */
			default:
				usage("too many -f options");
			}
			break;
		case 'm':
			merge = true;
			break;
		case 's':
			sorted = normal_sort;
			break;
		case 'S':
			sorted = reverse_sort;
			break;
		case 'c':
			complete = true;
			break;
		case 'I':
			info = true;
			break;
		case 'v':
			report_version();
			my_exit(0);
		case 'q':
			quiet = true;
			break;
		case 'h':
			help();
			my_exit(0);
		default:
			usage("unrecognized option");
		}
	}
	argc -= optind;
	if (argc != 0)
		usage("there are no non-option arguments to this program");
	argv = NULL;

	/* recondition various options for HTML use. */
	if (thing != NULL)
		escape(&thing);
	if (rrtype != NULL)
		escape(&rrtype);
	if (bailiwick != NULL)
		escape(&bailiwick);
	if (pfxlen != NULL)
		escape(&pfxlen);
	if (output_limit == -1 && query_limit != -1 && !merge)
		output_limit = query_limit;

	/* optionally dump program options as interpreted. */
	if (debug_level >= 1) {
		if (thing != NULL)
			debug(true, "thing = '%s'\n", thing);
		if (rrtype != NULL)
			debug(true, "type = '%s'\n", rrtype);
		if (bailiwick != NULL)
			debug(true, "bailiwick = '%s'\n", bailiwick);
		if (pfxlen != NULL)
			debug(true, "pfxlen = '%s'\n", pfxlen);
		if (after != 0)
			debug(true, "after = %ld : %s\n",
			      after, time_str(after, false));
		if (before != 0)
			debug(true, "before = %ld : ",
			      before, time_str(before, false));
		if (query_limit != -1)
			debug(true, "query_limit = %ld\n", query_limit);
		if (output_limit != -1)
			debug(true, "output_limit = %ld\n", output_limit);
		debug(true, "batching=%d, merge=%d\n",
		      batching != false, merge);
	}

	/* validate some interrelated options. */
	if (after != 0 && before != 0) {
		if (after > 0 && before > 0 && after > before)
			usage("-A -B requires after <= before (for now)");
		if (sorted == no_sort && json_fd == -1 &&
		    !complete && !quiet)
		{
			fprintf(stderr,
				"%s: warning: -A and -B w/o -c requires"
				" sorting for dedup, so turning on -S here.\n",
				program_name);
			sorted = reverse_sort;
		}
	}
	if (complete && !after && !before)
		usage("-c without -A or -B makes no sense.");
	if (merge) {
		switch (batching) {
		case batch_none:
			usage("using -m without -f makes no sense.");
		case batch_original:
			break;
		case batch_verbose:
			usage("using -m with more than one -f makes no sense.");
		}
	}
	if (nkeys > 0 && sorted == no_sort)
		usage("using -k without -s or -S makes no sense.");
	if (nkeys < MAX_KEYS && sorted != no_sort) {
		/* if sorting, all keys must be specified, to enable -u. */
		if (find_sort_key("first") == NULL)
			(void) add_sort_key("first");
		if (find_sort_key("last") == NULL)
			(void) add_sort_key("last");
		if (find_sort_key("count") == NULL)
			(void) add_sort_key("count");
		if (find_sort_key("name") == NULL)
			(void) add_sort_key("name");
		if (find_sort_key("data") == NULL)
			(void) add_sort_key("data");
	}

	assert(chosen_verb != NULL);
	if (chosen_verb->validate_cmd_opts != NULL)
		(*chosen_verb->validate_cmd_opts)();
	if (sys->validate_verb != NULL) {
		const char *msg = sys->validate_verb(chosen_verb->cmd_opt_val);
		if (msg != NULL)
			usage(msg);
	}

	/* get some input from somewhere, and use it to drive our output. */
	if (json_fd != -1) {
		if (mode != no_mode)
			usage("can't mix -n, -r, -i, or -R with -J");
		if (batching != batch_none)
			usage("can't mix -f with -J");
		if (bailiwick != NULL)
			usage("can't mix -b with -J");
		if (info)
			usage("can't mix -I with -J");
		if (rrtype != NULL)
			usage("can't mix -t with -J");
		if (chosen_verb != &verbs[DEFAULT_VERB])
			usage("can't mix -V with -J");
		if (sys != &pdns_systems[DEFAULT_SYS])
			usage("can't mix -u with -J");
		if (max_count > 0)
			usage("can't mix -M with -J");
		if (gravel)
			usage("can't mix -g with -J");
		if (offset != 0)
			usage("can't mix -O with -J");
		ruminate_json(json_fd, after, before);
		close(json_fd);
	} else if (batching != batch_none) {
		if (mode != no_mode)
			usage("can't mix -n, -r, -i, or -R with -f");
		if (bailiwick != NULL)
			usage("can't mix -b with -f");
		if (rrtype != NULL)
			usage("can't mix -t with -f");
		if (info)
			usage("can't mix -I with -f");
		server_setup();
		make_curl();
		do_batch(stdin, after, before);
		unmake_curl();
	} else if (info) {
		if (mode != no_mode)
			usage("can't mix -n, -r, -i, or -R with -I");
		if (pres != present_text && pres != present_json)
			usage("info must be presented in json or text format");
		if (bailiwick != NULL)
			usage("can't mix -b with -I");
		if (rrtype != NULL)
			usage("can't mix -t with -I");
		if (sys->request_info == NULL || sys->write_info == NULL)
			usage("there is no 'info' for this service");
		server_setup();
		make_curl();
		sys->request_info();
		unmake_curl();
	} else {
		struct query q;

		if (mode == no_mode)
			usage("must specify -r, -n, -i, or -R"
			      " unless -f or -J is used");
		if (bailiwick != NULL) {
			if (mode == ip_mode)
				usage("can't mix -b with -i");
			if (mode == raw_rrset_mode)
				usage("can't mix -b with -R");
			if (mode == raw_name_mode)
				usage("can't mix -b with -N");
			if (mode == name_mode)
				usage("can't mix -b with -n");
		}
		if (mode == ip_mode && rrtype != NULL)
			usage("can't mix -i with -t");

		q = (struct query) {
			.mode = mode,
			.thing = thing,
			.rrtype = rrtype,
			.bailiwick = bailiwick,
			.pfxlen = pfxlen,
			.after = after,
			.before = before
		};
		server_setup();
		make_curl();
		pdns_query(&q);
		unmake_curl();
	}

	/* clean up and go. */
	DESTROY(thing);
	DESTROY(rrtype);
	DESTROY(bailiwick);
	DESTROY(pfxlen);
	my_exit(exit_code);
}

/* Private. */

/* help -- display a brief usage-help text; then exit.
 *
 * this goes to stdout since we can expect it not to be piped unless to $PAGER.
 */
static void
help(void) {
	pdns_sys_t t;
	verb_t v;

	printf("usage: %s [-cdfghIjmqSsUv] [-p dns|json|csv]\n", program_name);
	puts("\t[-k (first|last|count|name|data)[,...]]\n"
	     "\t[-l QUERY-LIMIT] [-L OUTPUT-LIMIT] [-A after] [-B before]\n"
	     "\t[-u system] [-O offset] [-V verb] [-M max_count] {\n"
	     "\t\t-f |\n"
	     "\t\t-J inputfile |\n"
	     "\t\t[-t rrtype] [-b bailiwick] {\n"
	     "\t\t\t-r OWNER[/TYPE[/BAILIWICK]] |\n"
	     "\t\t\t-n NAME[/TYPE] |\n"
	     "\t\t\t-i IP[/PFXLEN] |\n"
	     "\t\t\t-N RAW-NAME-DATA[/TYPE]\n"
	     "\t\t\t-R RAW-OWNER-DATA[/TYPE[/BAILIWICK]]\n"
	     "\t\t}\n"
	     "\t}");
	puts("for -A and -B, use absolute format YYYY-MM-DD[ HH:MM:SS],\n"
	     "\tor relative format %dw%dd%dh%dm%ds.\n"
	     "use -c to get complete (strict) time matching for -A and -B.\n"
	     "use -d one or more times to ramp up the diagnostic output.\n"
	     "for -f, stdin must contain lines of the following forms:\n"
	     "\t  rrset/name/NAME[/TYPE[/BAILIWICK]]\n"
	     "\t  rrset/raw/HEX-PAIRS[/RRTYPE[/BAILIWICK]]\n"
	     "\t  rdata/name/NAME[/TYPE]\n"
	     "\t  rdata/ip/ADDR[,PFXLEN]\n"
	     "\t  rdata/raw/HEX-PAIRS[/RRTYPE]\n"
	     "\t  (output format will be determined by -p, "
	     "using --\\n framing.\n"
	     "use -g to get graveled results.\n"
	     "use -h to reliably display this helpful text.\n"
	     "use -I to see a system-specific account/key summary.\n"
	     "for -J, input format is newline-separated JSON, "
	     "as from -j output.\n"
	     "use -j as a synonym for -p json.\n"
	     "use -M # to end a summarize op when count exceeds threshold.\n"
	     "use -m with -f to merge all answers into a single result.\n"
	     "use -O # to skip this many results in what is returned.\n"
	     "use -q for warning reticence.\n"
	     "use -s to sort in ascending order, "
	     "or -S for descending order.\n"
	     "\t-s/-S can be repeated before several -k arguments.\n"
	     "use -U to turn off SSL certificate verification.\n"
	     "use -v to show the program version.");
	puts("for -u, system must be one of:");
	for (t = pdns_systems; t->name != NULL; t++)
		printf("\t%s\n", t->name);
	puts("for -V, verb must be one of:");
	for (v = verbs; v->cmd_opt_val != NULL; v++)
		printf("\t%s\n", v->cmd_opt_val);
	puts("\nGetting Started:\n"
	     "\tAdd your API key to ~/.dnsdb-query.conf like this:\n"
	     "\t\tAPIKEY=\"YOURAPIKEYHERE\"");
	printf("\nTry   man %s  for full documentation.\n", program_name);
}

static void
report_version(void) {
	printf("%s: version %s\n", program_name, id_version);
}

/* debug -- at the moment, dump to stderr.
 */
static void
debug(bool want_header, const char *fmtstr, ...) {
	va_list ap;

	va_start(ap, fmtstr);
	if (want_header)
		fputs("debug: ", stderr);
	vfprintf(stderr, fmtstr, ap);
	va_end(ap);
}	

/* usage -- display a usage error message, brief usage help text; then exit.
 *
 * this goes to stderr in case stdout has been piped or redirected.
 */
static __attribute__((noreturn)) void
usage(const char *fmtstr, ...) {
	va_list ap;

	va_start(ap, fmtstr);
	fputs("error: ", stderr);
	vfprintf(stderr, fmtstr, ap);
	va_end(ap);
	fputs("\n\n", stderr);
	fprintf(stderr,
		"try   %s -h   for a short description of program usage.\n",
		program_name);
	my_exit(1);
}

/* my_exit -- close or destroy global objects, then exit.
 */
static __attribute__((noreturn)) void
my_exit(int code) {
	int n;

	/* writers and readers which are still known, must be freed. */
	unmake_writers();

	/* if curl is operating, it must be shut down. */
	unmake_curl();

	/* globals which may have been initialized, are to be freed. */
	sys->destroy();

	/* sort key specifications and computations, are to be freed. */
	for (n = 0; n < nkeys; n++) {
		DESTROY(keys[n].specified);
		DESTROY(keys[n].computed);
	}

	/* terminate process. */
	DEBUG(1, true, "about to call exit(%d)\n", code);
	exit(code);
}

/* my_panic -- display an error on diagnostic output stream, exit ungracefully
 */
static __attribute__((noreturn)) void
my_panic(bool want_perror, const char *s) {
	fprintf(stderr, "%s: ", program_name);
	if (want_perror)
		perror(s);
	else
		fprintf(stderr, "%s\n", s);
	my_exit(1);
}

/* parse a base 10 long value.	Return true if ok, else return false.
 */
static bool
parse_long(const char *in, long *out) {
	char *ep;
	long result = strtol(in, &ep, 10);

	if ((errno == ERANGE && (result == LONG_MAX || result == LONG_MIN)) ||
	    (errno != 0 && result == 0) ||
	    (ep == in))
		return false;
	*out = result;
	return true;
}

/* validate_cmd_opts_lookup -- validate command line options for
 * a lookup verb
 */
static void
validate_cmd_opts_lookup(void) {
	/* TODO too many local variables would need to be global to check
	 * more here
	 */
	if (max_count > 0)
		usage("max_count only allowed for a summarize verb");
}

/* validate_cmd_opts_summarize -- validate command line options for
 * a summarize verb
 */
static void
validate_cmd_opts_summarize(void) {
	/* Remap the presentation format functions for the summarize variants */
	if (pres == present_json)
		pres = present_json_summarize;
	else if (pres == present_csv)
		pres = present_csv_summarize;
	else
		pres = present_text_summarize; /* default to text format */

	if (sorted != no_sort)
		usage("Sorting with a summarize verb makes no sense");
	/*TODO add more validations? */
}

/* or_else -- return one pointer or else the other. */
static const char *
or_else(const char *p, const char *or_else) {
	if (p != NULL)
		return p;
	return or_else;
}

/* add_sort_key -- add a key for use by POSIX sort.
 */
static const char *
add_sort_key(const char *tok) {
	const char *key = NULL;
	char *computed;
	int x;

	if (nkeys == MAX_KEYS)
		return ("too many sort keys given.");
	if (strcasecmp(tok, "first") == 0) {
		key = "-k1n";
	} else if (strcasecmp(tok, "last") == 0) {
		key = "-k2n";
	} else if (strcasecmp(tok, "count") == 0) {
		key = "-k3n";
	} else if (strcasecmp(tok, "name") == 0) {
		key = "-k4";
		sort_byname = true;
	} else if (strcasecmp(tok, "data") == 0) {
		key = "-k5";
		sort_bydata = true;
	}
	if (key == NULL)
		return ("key must be one of first, "
			"last, count, name, or data");
	x = asprintf(&computed, "%s%s", key,
		     sorted == reverse_sort ? "r" : "");
	if (x < 0)
		my_panic(true, "asprintf");
	keys[nkeys++] = (struct sortkey){strdup(tok), computed};
	return (NULL);
}

/* find_sort_key -- return pointer to a sort key, or NULL if it's not specified
 */
static sortkey_ct
find_sort_key(const char *tok) {
	int n;

	for (n = 0; n < nkeys; n++) {
		if (strcmp(keys[n].specified, tok) == 0)
			return (&keys[n]);
	}
	return (NULL);
}

/* find_pdns -- locate a pdns system's metadata by name.
 */
static pdns_sys_t
find_system(const char *name) {
	pdns_sys_t t;

	for (t = pdns_systems; t->name != NULL; t++)
		if (strcasecmp(t->name, name) == 0)
			return (t);
	return (NULL);
}

/* find_verb -- locate a verb by option parameter
 */
static verb_t
find_verb(const char *option) {
	verb_t v;

	for (v = verbs; v->cmd_opt_val != NULL; v++)
		if (strcasecmp(option, v->cmd_opt_val) == 0)
			return (v);
	return (NULL);
}

/* server_setup -- learn the server name and API key by various means.
 */
static void
server_setup(void) {
	read_configs();
	read_environ();
}

/* read_configs -- try to find a config file in static path, then parse it.
 */
static void
read_configs(void) {
	const char * const *conf;
	char *cf = NULL;

	for (conf = conf_files; *conf != NULL; conf++) {
		wordexp_t we;

		wordexp(*conf, &we, WRDE_NOCMD);
		cf = strdup(we.we_wordv[0]);
		wordfree(&we);
		if (access(cf, R_OK) == 0) {
			DEBUG(1, true, "conf found: '%s'\n", cf);
			break;
		}
		DESTROY(cf);
	}
	if (cf != NULL) {
		char *cmd, *line;
		size_t n;
		int x, l;
		FILE *f;

		x = asprintf(&cmd,
			     ". %s;"
			     "echo apikey $APIKEY;"
			     "echo server $DNSDB_SERVER;"
#if WANT_PDNS_CIRCL
			     "echo circla $CIRCL_AUTH;"
			     "echo circls $CIRCL_SERVER;"
#endif
			     "exit", cf);
		DESTROY(cf);
		if (x < 0)
			my_panic(true, "asprintf");
		f = popen(cmd, "r");
		if (f == NULL) {
			fprintf(stderr, "%s: [%s]: %s",
				program_name, cmd, strerror(errno));
			DESTROY(cmd);
			my_exit(1);
		}
		DEBUG(1, true, "conf cmd = '%s'\n", cmd);
		DESTROY(cmd);
		line = NULL;
		n = 0;
		l = 0;
		while (getline(&line, &n, f) > 0) {
			char **pp, *tok1, *tok2;
			char *saveptr = NULL;

			l++;
			if (strchr(line, '\n') == NULL) {
				fprintf(stderr,
					"%s: conf line #%d: too long\n",
					program_name, l);
				my_exit(1);
			}
			tok1 = strtok_r(line, "\040\012", &saveptr);
			tok2 = strtok_r(NULL, "\040\012", &saveptr);
			if (tok1 == NULL) {
				fprintf(stderr,
					"%s: conf line #%d: malformed\n",
					program_name, l);
				my_exit(1);
			}
			if (tok2 == NULL)
				continue;

			DEBUG(1, true, "line #%d: sets %s\n", l, tok1);
			pp = NULL;
			if (strcmp(tok1, "apikey") == 0) {
				pp = &api_key;
			} else if (strcmp(tok1, "server") == 0) {
				pp = &dnsdb_base_url;
#if WANT_PDNS_CIRCL
			} else if (strcmp(tok1, "circla") == 0) {
				pp = &circl_authinfo;
			} else if (strcmp(tok1, "circls") == 0) {
				pp = &circl_base_url;
#endif
			} else
				abort();
			DESTROY(*pp);
			*pp = strdup(tok2);
		}
		DESTROY(line);
		pclose(f);
	}
}


/* do_batch -- implement "filter" mode, reading commands from a batch file.
 *
 * the 'after' and 'before' arguments are from -A and -B and are defaults.
 */
static void
do_batch(FILE *f, u_long after, u_long before) {
	writer_t writer = NULL;
	char *command = NULL;
	size_t n = 0;

	/* if merging, start a writer. */
	if (merge)
		writer = writer_init(after, before);

	while (getline(&command, &n, f) > 0) {
		const char *msg;
		struct query q;
		char *nl;

		/* the last line of the file may not have a newline. */
		nl = strchr(command, '\n');
		if (nl != NULL)
			*nl = '\0';
		
		DEBUG(1, true, "do_batch(%s)\n", command);

		/* if not merging, start a writer here instead. */
		if (!merge) {
			writer = writer_init(after, before);
			/* only verbose batching shows query startups. */
			if (batching == batch_verbose)
				fprintf(stdout, "++ %s\n", command);
		}

		/* crack the batch line if possible. */
		msg = batch_parse(command, &q);
		if (msg != NULL) {
			writer_status(writer, "PARSE", msg);
		} else {
			/* manage batch-level defaults as -A and -B. */
			if (q.after == 0)
				q.after = after;
			if (q.before == 0)
				q.before = before;

			/* start one or two curl jobs based on this search. */
			query_launcher((query_ct)&q, writer);

			/* if merging, drain some jobs; else, drain all jobs. */
			if (merge) {
				io_engine(MAX_JOBS);
			} else {
				io_engine(0);
			}
		}
		if (writer->status != NULL && batching != batch_verbose) {
			assert(writer->message != NULL);
			fprintf(stderr, "%s: batch line status: %s (%s)\n",
				program_name, writer->status, writer->message);
		}

		/* think about showing the end-of-object separator. */
		if (!merge) {
			switch (batching) {
			case batch_none:
				break;
			case batch_original:
				fprintf(stdout, "--\n");
				break;
			case batch_verbose:
				fprintf(stdout, "-- %s (%s)\n",
					or_else(writer->status, "NOERROR"),
					or_else(writer->message, "no error"));
				break;
			default:
				abort();
			}
			fflush(stdout);
			writer_fini(writer);
			writer = NULL;
		}
	}
	DESTROY(command);
	
	/* if merging, run remaining jobs to completion, then finish up. */
	if (merge) {
		io_engine(0);
		writer_fini(writer);
		writer = NULL;
	}
}

/* batch_parse -- turn one line from a -f batch into a (struct query).
 */
static const char *
batch_parse(char *line, query_t qp) {
	struct query q = (struct query) { };
	char *saveptr = NULL;
	char *t;
	
	if ((t = strtok_r(line, "/", &saveptr)) == NULL)
		return "too few terms";
	if (strcmp(t, "rrset") == 0) {
		if ((t = strtok_r(NULL, "/", &saveptr)) == NULL)
			return "missing term after 'rrset/'";
		if (strcmp(t, "name") == 0) {
			q.mode = rrset_mode;
			if ((t = strtok_r(NULL, "/", &saveptr)) == NULL)
				return "missing term after 'rrset/name/'";
			q.thing = t;
			if ((t = strtok_r(NULL, "/", &saveptr)) != NULL) {
				q.rrtype = t;
				if ((t = strtok_r(NULL, "/", &saveptr))
				    != NULL)
				{
					q.bailiwick = t;
				}
			}
		} else if (strcmp(t, "raw") == 0) {
			q.mode = raw_rrset_mode;
			if ((t = strtok_r(NULL, "/", &saveptr)) == NULL)
				return "missing term after 'rrset/raw/'";
			q.thing = t;
			if ((t = strtok_r(NULL, "/", &saveptr)) != NULL) {
				q.rrtype = t;
				if ((t = strtok_r(NULL, "/", &saveptr))
				    != NULL)
				{
					q.bailiwick = t;
				}
			}
		} else {
			return "unrecognized term after 'rrset/'";
		}
	} else if (strcmp(t, "rdata") == 0) {
		if ((t = strtok_r(NULL, "/", &saveptr)) == NULL)
			return "missing term after 'rdata/'";
		if (strcmp(t, "name") == 0) {
			q.mode = name_mode;
			if ((t = strtok_r(NULL, "/", &saveptr)) == NULL)
				return "missing term after 'rdata/name/'";
			q.thing = t;
			if ((t = strtok_r(NULL, "/", &saveptr)) != NULL) {
				q.rrtype = t;
			}
		} else if (strcmp(t, "raw") == 0) {
			q.mode = raw_name_mode;
			if ((t = strtok_r(NULL, "/", &saveptr)) == NULL)
				return "missing term after 'rdata/raw/'";
			q.thing = t;
			if ((t = strtok_r(NULL, "/", &saveptr)) != NULL) {
				q.rrtype = t;
			}
		} else if (strcmp(t, "ip") == 0) {
			q.mode = ip_mode;
			if ((t = strtok_r(NULL, "/", &saveptr)) == NULL)
				return "missing term after 'rdata/ip/'";
			q.thing = t;
		} else {
			return "unrecognized term after 'rdata/'";
		}
	} else {
		return "unrecognized initial term";
	}
	t = strtok_r(NULL, "/", &saveptr);
	if (t != NULL)
		return "extra garbage";
	*qp = q;
	return NULL;
}

/* makepath -- make a RESTful URI that describes these search parameters.
 */
static char *
makepath(mode_e mode, const char *name, const char *rrtype,
	 const char *bailiwick, const char *pfxlen)
{
	char *command;
	int x;

	switch (mode) {
	case rrset_mode:
		if (rrtype != NULL && bailiwick != NULL)
			x = asprintf(&command, "rrset/name/%s/%s/%s",
				     name, rrtype, bailiwick);
		else if (rrtype != NULL)
			x = asprintf(&command, "rrset/name/%s/%s",
				     name, rrtype);
		else if (bailiwick != NULL)
			x = asprintf(&command, "rrset/name/%s/ANY/%s",
				     name, bailiwick);
		else
			x = asprintf(&command, "rrset/name/%s",
				     name);
		if (x < 0)
			my_panic(true, "asprintf");
		break;
	case name_mode:
		if (rrtype != NULL)
			x = asprintf(&command, "rdata/name/%s/%s",
				     name, rrtype);
		else
			x = asprintf(&command, "rdata/name/%s",
				     name);
		if (x < 0)
			my_panic(true, "asprintf");
		break;
	case ip_mode:
		if (pfxlen != NULL)
			x = asprintf(&command, "rdata/ip/%s,%s",
				     name, pfxlen);
		else
			x = asprintf(&command, "rdata/ip/%s",
				     name);
		if (x < 0)
			my_panic(true, "asprintf");
		break;
	case raw_rrset_mode:
		if (rrtype != NULL)
			x = asprintf(&command, "rrset/raw/%s/%s",
				     name, rrtype);
		else
			x = asprintf(&command, "rrset/raw/%s",
				     name);
		if (x < 0)
			my_panic(true, "asprintf");
		break;
	case raw_name_mode:
		if (rrtype != NULL)
			x = asprintf(&command, "rdata/raw/%s/%s",
				     name, rrtype);
		else
			x = asprintf(&command, "rdata/raw/%s",
				     name);
		if (x < 0)
			my_panic(true, "asprintf");
		break;
	case no_mode:
		/*FALLTHROUGH*/
	default:
		abort();
	}
	return (command);
}

/* make_curl -- perform global initializations of libcurl.
 */
static void
make_curl(void) {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl_cleanup_needed++;
	multi = curl_multi_init();
	if (multi == NULL) {
		fprintf(stderr, "%s: curl_multi_init() failed\n",
			program_name);
		my_exit(1);
	}
}

/* unmake_curl -- clean up and discard libcurl's global state.
 */
static void
unmake_curl(void) {
	if (multi != NULL) {
		curl_multi_cleanup(multi);
		multi = NULL;
	}
	if (curl_cleanup_needed) {
		curl_global_cleanup();
		curl_cleanup_needed = 0;
	}
}

/* pdns_query -- launch one or more libcurl jobs to fulfill this DNSDB query.
 *
 * this is the non-batch path where only one query is made before exit.
 */
static void
pdns_query(query_ct qp) {
	writer_t writer;

	/* start a writer, which might be format functions, or POSIX sort. */
	writer = writer_init(qp->after, qp->before);

	/* start a small and finite number of readers on that writer. */
	query_launcher(qp, writer);
	
	/* run all jobs to completion. */
	io_engine(0);

	/* stop the writer, which might involve reading POSIX sort's output. */
	writer_fini(writer);
}

/* query_launcher -- fork off some curl jobs via launch() for this query.
 */
static void
query_launcher(query_ct qp, writer_t writer) {
	char *command;
	
	command = makepath(qp->mode, qp->thing, qp->rrtype,
			   qp->bailiwick, qp->pfxlen);

	/* figure out from time fencing which job(s) we'll be starting.
	 *
	 * the 4-tuple is: first_after, first_before, last_after, last_before
	 */
	if (qp->after != 0 && qp->before != 0) {
		if (complete) {
			/* each db tuple must be enveloped by time fence. */
			launch(command, writer, qp->after, 0, 0, qp->before);
		} else {
			/* we need tuples that end after fence start... */
			launch(command, writer, 0, 0, qp->after, 0);
			/* ...and that begin before the time fence end. */
			launch(command, writer, 0, qp->before, 0, 0);
			/* and we will filter in reader_func() to
			 * select only those tuples which either:
			 * ...(start within), or (end within), or
			 * ...(start before and end after).
			 */
		}
	} else if (qp->after != 0) {
		if (complete) {
			/* each db tuple must begin after the fence-start. */
			launch(command, writer, qp->after, 0, 0, 0);
		} else {
			/* each db tuple must end after the fence-start. */
			launch(command, writer, 0, 0, qp->after, 0);
		}
	} else if (qp->before != 0) {
		if (complete) {
			/* each db tuple must end before the fence-end. */
			launch(command, writer, 0, 0, 0, qp->before);
		} else {
			/* each db tuple must begin before the fence-end. */
			launch(command, writer, 0, qp->before, 0, 0);
		}
	} else {
		/* no time fencing. */
		launch(command, writer, 0, 0, 0, 0);
	}
	DESTROY(command);
}

/* launch -- actually launch a query job, given a command and time fences.
 */
static void
launch(const char *command, writer_t writer,
       u_long first_after, u_long first_before,
       u_long last_after, u_long last_before)
{
	char *url, *tmp, sep;
	int x;

	url = sys->url(command, &sep);
	if (url == NULL)
		my_exit(1);

	if (query_limit != -1) {
		x = asprintf(&tmp, "%s%c" "limit=%ld", url, sep, query_limit);
		if (x < 0) {
			perror("asprintf");
			DESTROY(url);
			my_exit(1);
		}
		DESTROY(url);
		url = tmp;
		tmp = NULL;
		sep = '&';
	}
	if (first_after != 0) {
		x = asprintf(&tmp, "%s%c" "time_first_after=%lu",
			     url, sep, (u_long)first_after);
		if (x < 0) {
			perror("asprintf");
			DESTROY(url);
			my_exit(1);
		}
		DESTROY(url);
		url = tmp;
		tmp = NULL;
		sep = '&';
	}
	if (first_before != 0) {
		x = asprintf(&tmp, "%s%c" "time_first_before=%lu",
			     url, sep, (u_long)first_before);
		if (x < 0) {
			perror("asprintf");
			DESTROY(url);
			my_exit(1);
		}
		DESTROY(url);
		url = tmp;
		tmp = NULL;
		sep = '&';
	}
	if (last_after != 0) {
		x = asprintf(&tmp, "%s%c" "time_last_after=%lu",
			     url, sep, (u_long)last_after);
		if (x < 0) {
			perror("asprintf");
			DESTROY(url);
			my_exit(1);
		}
		DESTROY(url);
		url = tmp;
		tmp = NULL;
		sep = '&';
	}
	if (last_before != 0) {
		x = asprintf(&tmp, "%s%c" "time_last_before=%lu",
			     url, sep, (u_long)last_before);
		if (x < 0) {
			perror("asprintf");
			DESTROY(url);
			my_exit(1);
		}
		DESTROY(url);
		url = tmp;
		tmp = NULL;
		sep = '&';
	}
	DEBUG(1, true, "url [%s]\n", url);

	reader_launch(writer, url);
}

/* reader_launch -- given a url, tell libcurl to go fetch it.
 */
static void
reader_launch(writer_t writer, char *url) {
	reader_t reader = NULL;
	CURLMcode res;

	DEBUG(2, true, "reader_launch(%s)\n", url);
	CREATE(reader, sizeof *reader);
	reader->writer = writer;
	writer = NULL;
	reader->easy = curl_easy_init();
	if (reader->easy == NULL) {
		/* an error will have been output by libcurl in this case. */
		DESTROY(reader);
		DESTROY(url);
		my_exit(1);
	}
	reader->url = url;
	url = NULL;
	curl_easy_setopt(reader->easy, CURLOPT_URL, reader->url);
	if (donotverify) {
		curl_easy_setopt(reader->easy, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(reader->easy, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	sys->auth(reader);
	reader->hdrs = curl_slist_append(reader->hdrs, json_header);
	curl_easy_setopt(reader->easy, CURLOPT_HTTPHEADER, reader->hdrs);
	curl_easy_setopt(reader->easy, CURLOPT_WRITEFUNCTION, writer_func);
	curl_easy_setopt(reader->easy, CURLOPT_WRITEDATA, reader);
#if CURL_AT_LEAST_VERSION(7,42,0)
	/* do not allow curl to swallow /./ and /../ in our URLs */
	curl_easy_setopt(reader->easy, CURLOPT_PATH_AS_IS, 1L);
#endif /* CURL_AT_LEAST_VERSION */
	if (debug_level >= 3)
		curl_easy_setopt(reader->easy, CURLOPT_VERBOSE, 1L);

	/* linked-list insert. */
	reader->next = reader->writer->readers;
	reader->writer->readers = reader;

	res = curl_multi_add_handle(multi, reader->writer->readers->easy);
	if (res != CURLM_OK) {
		fprintf(stderr, "%s: curl_multi_add_handle() failed: %s\n",
			program_name, curl_multi_strerror(res));
		my_exit(1);
	}
}

/* reader_reap -- reap one reader.
 */
static void
reader_reap(reader_t reader) {
	if (reader->easy != NULL) {
		curl_multi_remove_handle(multi, reader->easy);
		curl_easy_cleanup(reader->easy);
		reader->easy = NULL;
	}
	if (reader->hdrs != NULL) {
		curl_slist_free_all(reader->hdrs);
		reader->hdrs = NULL;
	}
	DESTROY(reader->url);
	DESTROY(reader);
}

/* ruminate_json -- process a json file from the filesys rather than the API.
 */
static void
ruminate_json(int json_fd, u_long after, u_long before) {
	reader_t reader = NULL;
	void *buf = NULL;
	writer_t writer;
	ssize_t len;

	writer = writer_init(after, before);
	CREATE(reader, sizeof(struct reader));
	reader->writer = writer;
	writer->readers = reader;
	reader = NULL;
	CREATE(buf, ideal_buffer);
	while ((len = read(json_fd, buf, sizeof buf)) > 0) {
		writer_func(buf, 1, (size_t)len, writer->readers);
	}
	DESTROY(buf);
	writer_fini(writer);
	writer = NULL;
}

/* writer_init -- instantiate a writer, which may involve forking a "sort".
 */
static writer_t
writer_init(u_long after, u_long before) {
	writer_t writer = NULL;

	CREATE(writer, sizeof(struct writer));

	if (sorted != no_sort) {
		/* sorting involves a subprocess (POSIX sort(1) command),
		 * which will by definition not output anything until
		 * after it receives EOF. this means we can pipe both
		 * to its stdin and from its stdout, without risk of
		 * deadlock. it also means a full store-and-forward of
		 * the result, which increases latency to the first
		 * output for our user.
		 */
		int p1[2], p2[2];

		if (pipe(p1) < 0 || pipe(p2) < 0)
			my_panic(true, "pipe");
		if ((writer->sort_pid = fork()) < 0)
			my_panic(true, "fork");
		if (writer->sort_pid == 0) {
			char *sort_argv[3+MAX_KEYS], **sap;
			int n;

			if (dup2(p1[0], STDIN_FILENO) < 0 ||
			    dup2(p2[1], STDOUT_FILENO) < 0) {
				perror("dup2");
				_exit(1);
			}
			close(p1[0]); close(p1[1]);
			close(p2[0]); close(p2[1]);
			sap = sort_argv;
			*sap++ = strdup("sort");
			*sap++ = strdup("-u");
			for (n = 0; n < nkeys; n++)
				*sap++ = strdup(keys[n].computed);
			*sap++ = NULL;
			putenv(strdup("LC_ALL=C"));
			DEBUG(1, true, "\"%s\" args:", path_sort);
			for (sap = sort_argv; *sap != NULL; sap++)
				DEBUG(1, false, " [%s]", *sap);
			DEBUG(1, false, "\n");
			execve(path_sort, sort_argv, environ);
			perror("execve");
			for (sap = sort_argv; *sap != NULL; sap++)
				DESTROY(*sap);
			_exit(1);
		}
		close(p1[0]);
		writer->sort_stdin = fdopen(p1[1], "w");
		writer->sort_stdout = fdopen(p2[0], "r");
		close(p2[1]);
	}

	writer->after = after;
	writer->before = before;
	writer->next = writers;
	writers = writer;
	return (writer);
}

/* writer_status -- install a status code and description in a writer.
 */
static void
writer_status(writer_t writer, const char *status, const char *message) {
	assert((writer->status == NULL) == (writer->message == NULL));
	assert(writer->status == NULL);
	writer->status = strdup(status);
	writer->message = strdup(message);
}

/* writer_func -- process a block of json text, from filesys or API socket.
 */
static size_t
writer_func(char *ptr, size_t size, size_t nmemb, void *blob) {
	reader_t reader = (reader_t) blob;
	size_t bytes = size * nmemb;
	u_long after, before;
	FILE *outf;
	char *nl;

	DEBUG(3, true, "writer_func(%d, %d): %d\n",
	      (int)size, (int)nmemb, (int)bytes);

	reader->buf = realloc(reader->buf, reader->len + bytes);
	memcpy(reader->buf + reader->len, ptr, bytes);
	reader->len += bytes;

	/* when the reader is a live web result, emit
	 * !2xx errors and info payloads as reports.
	 */
	if (reader->easy != NULL) {
		if (reader->rcode == 0)
			curl_easy_getinfo(reader->easy,
					  CURLINFO_RESPONSE_CODE,
					  &reader->rcode);
		if (reader->rcode != 200) {
			char *message = strndup(reader->buf, reader->len);
			char *newline = strchr(message, '\n');
			if (newline != NULL)
				*newline = '\0';

			if (!reader->writer->once) {
				writer_status(reader->writer,
					      sys->status(reader),
					      message);
				if (!quiet) {
					char *url;
					
					curl_easy_getinfo(reader->easy,
							 CURLINFO_EFFECTIVE_URL,
							  &url);
					fprintf(stderr,
						"%s: warning: "
						"libcurl %ld [%s]\n",
						program_name, reader->rcode,
						url);
				}
				reader->writer->once = true;
			}
			if (!quiet)
				fprintf(stderr, "%s: warning: libcurl: [%s]\n",
					program_name, message);
			DESTROY(message);
			reader->buf[0] = '\0';
			reader->len = 0;
			return (bytes);
		}
	}

	after = reader->writer->after;
	before = reader->writer->before;
	outf = (sorted != no_sort) ? reader->writer->sort_stdin : stdout;

	while ((nl = memchr(reader->buf, '\n', reader->len)) != NULL) {
		size_t pre_len, post_len;

		if (info) {
			sys->write_info(reader);
			reader->buf[0] = '\0';
			reader->len = 0;
			return (bytes);
		}

		if (sorted == no_sort && output_limit != -1 &&
		    reader->writer->count >= output_limit)
		{
			DEBUG(1, true, "hit output limit %ld\n", output_limit);
			reader->buf[0] = '\0';
			reader->len = 0;
			return (bytes);
		}

		pre_len = (size_t)(nl - reader->buf);
		reader->writer->count += input_blob(reader->buf, pre_len,
						    after, before, outf);
		post_len = (reader->len - pre_len) - 1;
		memmove(reader->buf, nl + 1, post_len);
		reader->len = post_len;
	}
	return (bytes);
}

/* input_blob -- process one deblocked json blob as a counted string.
 */
static int
input_blob(const char *buf, size_t len,
	   u_long after, u_long before,
	   FILE *outf)
{
	const char *msg, *whynot;
	struct pdns_tuple tup;
	u_long first, last;
	int ret = 0;

	msg = tuple_make(&tup, buf, len);
	if (msg != NULL) {
		fputs(msg, stderr);
		fputc('\n', stderr);
		goto more;
	}

	/* there are two sets of timestamps in a tuple. we prefer
	 * the on-the-wire times to the zone times, when available.
	 */
	if (tup.time_first != 0 && tup.time_last != 0) {
		first = (u_long)tup.time_first;
		last = (u_long)tup.time_last;
	} else {
		first = (u_long)tup.zone_first;
		last = (u_long)tup.zone_last;
	}

	/* time fencing can in some cases (-A & -B w/o -c) require
	 * asking the server for more than we really want, and so
	 * we have to winnow it down upon receipt. (see also -J.)
	 */
	whynot = NULL;
	DEBUG(2, true, "filtering-- ");
	if (after != 0) {
		int first_vs_after, last_vs_after;

		first_vs_after = timecmp(first, after);
		last_vs_after = timecmp(last, after);
		DEBUG(2, false, "FvA %d LvA %d: ",
			 first_vs_after, last_vs_after);

		if (complete) {
			if (first_vs_after < 0) {
				whynot = "first is too early";
			}
		} else {
			if (last_vs_after < 0) {
				whynot = "last is too early";
			}
		}
	}
	if (before != 0) {
		int first_vs_before, last_vs_before;

		first_vs_before = timecmp(first, before);
		last_vs_before = timecmp(last, before);
		DEBUG(2, false, "FvB %d LvB %d: ",
			 first_vs_before, last_vs_before);

		if (complete) {
			if (last_vs_before > 0) {
				whynot = "last is too late";
			}
		} else {
			if (first_vs_before > 0) {
				whynot = "first is too late";
			}
		}
	}

	if (whynot == NULL) {
		DEBUG(2, false, "selected!\n");
	} else {
		DEBUG(2, false, "skipped (%s).\n", whynot);
	}
	DEBUG(3, true, "\tF..L = %s", time_str(first, false));
	DEBUG(3, false, " .. %s\n", time_str(last, false));
	DEBUG(3, true, "\tA..B = %s", time_str(after, false));
	DEBUG(3, false, " .. %s\n", time_str(before, false));
	if (whynot != NULL)
		goto next;

	if (sorted != no_sort) {
		/* POSIX sort is given five extra fields at the
		 * front of each line (first,last,count,name,data)
		 * which are accessed as -k1 .. -k5 on the
		 * sort command line. we strip them off later
		 * when reading the result back. the reason
		 * for all this PDP11-era logic is to avoid
		 * having to store the full result in memory.
		 */
		char *dyn_rrname = NULL, *dyn_rdata = NULL;
		if (sort_byname) {
			dyn_rrname = sortable_rrname(&tup);
			DEBUG(2, true, "dyn_rrname = '%s'\n", dyn_rrname);
		}
		if (sort_bydata) {
			dyn_rdata = sortable_rdata(&tup);
			DEBUG(2, true, "dyn_rdata = '%s'\n", dyn_rdata);
		}
		fprintf(outf, "%lu %lu %lu %s %s %*.*s\n",
			(unsigned long)first,
			(unsigned long)last,
			(unsigned long)tup.count,
			or_else(dyn_rrname, "n/a"),
			or_else(dyn_rdata, "n/a"),
			(int)len, (int)len, buf);
		DEBUG(2, true, "sort0: '%lu %lu %lu %s %s %*.*s'\n",
			 (unsigned long)first,
			 (unsigned long)last,
			 (unsigned long)tup.count,
			 or_else(dyn_rrname, "n/a"),
			 or_else(dyn_rdata, "n/a"),
			 (int)len, (int)len, buf);
		DESTROY(dyn_rrname);
		DESTROY(dyn_rdata);
	} else {
		(*pres)(&tup, buf, len, outf);
	}
	ret = 1;
 next:
	tuple_unmake(&tup);
 more:
	return (ret);
}

/* writer_fini -- stop a writer's readers, and perhaps execute a POSIX "sort".
 */
static void
writer_fini(writer_t writer) {
	/* unlink this writer from the global chain. */
	if (writers == writer) {
		writers = writer->next;
	} else {
		writer_t prev = NULL;
		writer_t temp;

		for (temp = writers; temp != NULL; temp = temp->next) {
			if (temp->next == writer) {
				prev = temp;
				break;
			}
		}
		assert(prev != NULL);
		prev->next = writer->next;
	}

	/* finish and close any readers still cooking. */
	while (writer->readers != NULL) {
		reader_t reader = writer->readers;

		/* release any buffered info. */
		DESTROY(reader->buf);
		if (reader->len != 0) {
			fprintf(stderr, "%s: warning: stranding %d octets!\n",
				program_name, (int)reader->len);
			reader->len = 0;
		}

		/* tear down any curl infrastructure on the reader & remove. */
		reader_t next = reader->next;
		reader_reap(reader);
		reader = NULL;
		writer->readers = next;
	}

	/* drain the sort if there is one. */
	if (writer->sort_pid != 0) {
		int status, count;
		char *line = NULL;
		size_t n = 0;

		/* when sorting, there has been no output yet. gather the
		 * intermediate representation from the POSIX sort stdout,
		 * skip over the sort keys we added earlier, and process.
		 */
		fclose(writer->sort_stdin);
		DEBUG(1, true, "closed sort_stdin, wrote %d objs\n",
			 writer->count);
		count = 0;
		while (getline(&line, &n, writer->sort_stdout) > 0) {
			/* if we're above the limit, ignore remaining output.
			 * this is nec'y to avoid SIGPIPE from sort if we were
			 * to close its stdout pipe without emptying it first.
			 */
			if (output_limit != -1 && count >= output_limit) {
				if (!writer->sort_killed) {
					kill(writer->sort_pid, SIGTERM);
					writer->sort_killed = true;
				}
				continue;
			}

			char *nl, *linep;
			const char *msg;
			struct pdns_tuple tup;

			if ((nl = strchr(line, '\n')) == NULL) {
				fprintf(stderr,
					"%s: warning: no \\n found in '%s'\n",
					program_name, line);
				continue;
			}
			linep = line;
			DEBUG(2, true, "sort1: '%*.*s'\n",
				 (int)(nl - linep),
				 (int)(nl - linep),
				 linep);
			/* skip sort keys (first, last, count, name, data). */
			if ((linep = strchr(linep, ' ')) == NULL) {
				fprintf(stderr,
					"%s: warning: no SP found in '%s'\n",
					program_name, line);
				continue;
			}
			linep += strspn(linep, " ");
			if ((linep = strchr(linep, ' ')) == NULL) {
				fprintf(stderr,
					"%s: warning: no second SP in '%s'\n",
					program_name, line);
				continue;
			}
			linep += strspn(linep, " ");
			if ((linep = strchr(linep, ' ')) == NULL) {
				fprintf(stderr,
					"%s: warning: no third SP in '%s'\n",
					program_name, line);
				continue;
			}
			linep += strspn(linep, " ");
			if ((linep = strchr(linep, ' ')) == NULL) {
				fprintf(stderr,
					"%s: warning: no fourth SP in '%s'\n",
					program_name, line);
				continue;
			}
			linep += strspn(linep, " ");
			if ((linep = strchr(linep, ' ')) == NULL) {
				fprintf(stderr,
					"%s: warning: no fifth SP in '%s'\n",
					program_name, line);
				continue;
			}
			linep += strspn(linep, " ");
			DEBUG(2, true, "sort2: '%*.*s'\n",
				 (int)(nl - linep),
				 (int)(nl - linep),
				 linep);
			msg = tuple_make(&tup, linep, (size_t)(nl - linep));
			if (msg != NULL) {
				fprintf(stderr,
					"%s: warning: tuple_make: %s\n",
					program_name, msg);
				continue;
			}
			(*pres)(&tup, linep, (size_t)(nl - linep), stdout);
			tuple_unmake(&tup);
			count++;
		}
		DESTROY(line);
		fclose(writer->sort_stdout);
		DEBUG(1, true, "closed sort_stdout, read %d objs (lim %ld)\n",
		      count, query_limit);
		if (waitpid(writer->sort_pid, &status, 0) < 0) {
			perror("waitpid");
		} else {
			if (!writer->sort_killed && status != 0)
				fprintf(stderr,
					"%s: warning: sort "
					"exit status is %u\n",
					program_name, (unsigned)status);
		}
	}

	/* drop message and status strings if present. */
	assert((writer->status != NULL) == (writer->message != NULL));
	if (writer->status != NULL)
		DESTROY(writer->status);
	if (writer->message != NULL)
		DESTROY(writer->message);

	DESTROY(writer);
}

static void
unmake_writers(void) {
	while (writers != NULL)
		writer_fini(writers);
}

/* io_engine -- let libcurl run until there are few enough outstanding jobs.
 */
static void
io_engine(int jobs) {
	int still, repeats, numfds;
	struct CURLMsg *cm;

	DEBUG(2, true, "io_engine(%d)\n", jobs);

	/* let libcurl run while there are too many jobs remaining. */
	still = 0;
	repeats = 0;
	while (curl_multi_perform(multi, &still) == CURLM_OK && still > jobs) {
		DEBUG(4, true, "...waiting (still %d)\n", still);
		numfds = 0;
		if (curl_multi_wait(multi, NULL, 0, 0, &numfds) != CURLM_OK)
			break;
		if (numfds == 0) {
			/* curl_multi_wait() can return 0 fds for no reason. */
			if (++repeats > 1) {
				struct timespec req, rem;

				req = (struct timespec){
					.tv_sec = 0,
					.tv_nsec = 100*1000*1000  // 100ms
				};
				while (nanosleep(&req, &rem) == EINTR) {
					/* as required by nanosleep(3). */
					req = rem;
				}
			}
		} else {
			repeats = 0;
		}
	}

	/* drain the response code reports. */
	still = 0;
	while ((cm = curl_multi_info_read(multi, &still)) != NULL) {
		if (cm->msg == CURLMSG_DONE && cm->data.result != CURLE_OK) {
			if (cm->data.result == CURLE_COULDNT_RESOLVE_HOST)
				fprintf(stderr,
					"%s: warning: libcurl failed since "
					"could not resolve host\n",
					program_name);
			else if (cm->data.result == CURLE_COULDNT_CONNECT)
				fprintf(stderr,
					"%s: warning: libcurl failed since "
					"could not connect\n",
					program_name);
			else
				fprintf(stderr,
					"%s: warning: libcurl failed with "
					"curl error %d\n",
					program_name, cm->data.result);
			exit_code = 1;
		}
		DEBUG(4, true, "...info read (still %d)\n", still);
	}
}

/* timecmp -- compare two absolute timestamps, give -1, 0, or 1.
 */
static int
timecmp(u_long a, u_long b) {
	if (a < b)
		return (-1);
	if (a > b)
		return (1);
	return (0);
}

/* time_str -- format one (possibly relative) timestamp (returns static string)
 */
static const char *
time_str(u_long x, bool iso8601fmt) {
	static char ret[sizeof "yyyy-mm-ddThh:mm:ssZ"];

	if (x == 0) {
		strcpy(ret, "0");
	} else {
		time_t t = (time_t)x;
		struct tm result, *y = gmtime_r(&t, &result);

		strftime(ret, sizeof ret, iso8601fmt ? "%FT%TZ" : "%F %T", y);
	}
	return ret;
}

/* time_get -- parse and return one (possibly relative) timestamp.
 */
static int
time_get(const char *src, u_long *dst) {
	struct tm tt;
	long long ll;
	u_long t;
	char *ep;

	memset(&tt, 0, sizeof tt);
	if (((ep = strptime(src, "%F %T", &tt)) != NULL && *ep == '\0') ||
	    ((ep = strptime(src, "%F", &tt)) != NULL && *ep == '\0'))
	{
		*dst = (u_long)(timegm(&tt));
		return (1);
	}
	ll = strtoll(src, &ep, 10);
	if (*src != '\0' && *ep == '\0') {
		if (ll < 0)
			*dst = (u_long)now.tv_sec - (u_long)imaxabs(ll);
		else
			*dst = (u_long)ll;
		return (1);
	}
	if (ns_parse_ttl(src, &t) == 0) {
		*dst = (u_long)now.tv_sec - t;
		return (1);
	}
	return (0);
}

/* escape -- HTML-encode a string, in place.
 */
static void
escape(char **src) {
	char *escaped;

	escaped = curl_escape(*src, (int)strlen(*src));
	if (escaped == NULL) {
		fprintf(stderr, "%s: curl_escape(%s) failed\n",
			program_name, *src);
		my_exit(1);
	}
	DESTROY(*src);
	*src = strdup(escaped);
	curl_free(escaped);
	escaped = NULL;
}

/* sortable_rrname -- return a POSIX-sort-collatable rendition of RR name+type.
 */
static char *
sortable_rrname(pdns_tuple_ct tup) {
	struct sortbuf buf = {NULL, 0};

	sortable_dnsname(&buf, tup->rrname);
	buf.base = realloc(buf.base, buf.size+1);
	buf.base[buf.size++] = '\0';
	return (buf.base);
}

/* sortable_rdata -- return a POSIX-sort-collatable rendition of RR data set.
 */
static char *
sortable_rdata(pdns_tuple_ct tup) {
	struct sortbuf buf = {NULL, 0};

	if (json_is_array(tup->obj.rdata)) {
		size_t slot, nslots;

		nslots = json_array_size(tup->obj.rdata);
		for (slot = 0; slot < nslots; slot++) {
			json_t *rr = json_array_get(tup->obj.rdata, slot);

			if (json_is_string(rr))
				sortable_rdatum(&buf, tup->rrtype,
						json_string_value(rr));
			else
				fprintf(stderr,
					"%s: warning: rdata slot "
					"is not a string\n",
					program_name);
		}
	} else {
		sortable_rdatum(&buf, tup->rrtype, tup->rdata);
	}
	buf.base = realloc(buf.base, buf.size+1);
	buf.base[buf.size++] = '\0';
	return (buf.base);
}

/* sortable_rdatum -- called only by sortable_rdata(), realloc and normalize.
 *
 * this converts (lossily) addresses into hex strings, and extracts the
 * server-name component of a few other types like MX. all other rdata
 * are left in their normal string form, because it's hard to know what
 * to sort by with something like TXT, and extracting the serial number
 * from an SOA using a language like C is a bit ugly.
 */
static void
sortable_rdatum(sortbuf_t buf, const char *rrtype, const char *rdatum) {
	if (strcmp(rrtype, "A") == 0) {
		u_char a[4];

		if (inet_pton(AF_INET, rdatum, a) != 1)
			memset(a, 0, sizeof a);
		sortable_hexify(buf, a, sizeof a);
	} else if (strcmp(rrtype, "AAAA") == 0) {
		u_char aaaa[16];

		if (inet_pton(AF_INET6, rdatum, aaaa) != 1)
			memset(aaaa, 0, sizeof aaaa);
		sortable_hexify(buf, aaaa, sizeof aaaa);
	} else if (strcmp(rrtype, "NS") == 0 ||
		   strcmp(rrtype, "PTR") == 0 ||
		   strcmp(rrtype, "CNAME") == 0)
	{
		sortable_dnsname(buf, rdatum);
	} else if (strcmp(rrtype, "MX") == 0 ||
		   strcmp(rrtype, "RP") == 0)
	{
		const char *space = strrchr(rdatum, ' ');

		if (space != NULL)
			sortable_dnsname(buf, space+1);
		else
			sortable_hexify(buf, (const u_char *)rdatum,
					strlen(rdatum));
	} else {
		sortable_hexify(buf, (const u_char *)rdatum, strlen(rdatum));
	}
}

static void
sortable_hexify(sortbuf_t buf, const u_char *src, size_t len) {
	size_t i;

	buf->base = realloc(buf->base, buf->size + len*2);
	for (i = 0; i < len; i++) {
		const char hex[] = "0123456789abcdef";
		unsigned int ch = src[i];

		buf->base[buf->size++] = hex[ch >> 4];
		buf->base[buf->size++] = hex[ch & 0xf];
	}
}

/* sortable_dnsname -- make a sortable dns name; destructive and lossy.
 *
 * to be lexicographically sortable, a dnsname has to be converted to
 * TLD-first, all uppercase letters must be converted to lower case,
 * and all characters except dots then converted to hexadecimal. this
 * transformation is for POSIX sort's use, and is irreversibly lossy.
 */
static void
sortable_dnsname(sortbuf_t buf, const char *name) {
	const char hex[] = "0123456789abcdef";
	size_t len, new_size;
	unsigned int dots;
	signed int m, n;
	char *p;

	/* to avoid calling realloc() on every label, count the dots. */
	for (dots = 0, len = 0; name[len] != '\0'; len++) {
		if (name[len] == '.')
			dots++;
	}

	/* collatable names are TLD-first, all lower case. */
	new_size = buf->size + len*2 - (size_t)dots;
	assert(new_size != 0);
	if (new_size != buf->size)
		buf->base = realloc(buf->base, new_size);
	p = buf->base + buf->size;
	for (m = (int)len - 1, n = m; m >= 0; m--) {
		/* note: actual presentation form names can have \. and \\,
		 * but we are destructive and lossy, and will ignore that.
		 */
		if (name[m] == '.') {
			int i;

			for (i = m+1; i <= n; i++) {
				int ch = tolower(name[i]);
				*p++ = hex[ch >> 4];
				*p++ = hex[ch & 0xf];
			}
			*p++ = '.';
			n = m-1;
		}
	}
	assert(m == -1);
	/* first label remains after loop. */
	for (m = 0; m <= n; m++) {
		int ch = tolower(name[m]);
		*p++ = hex[ch >> 4];
		*p++ = hex[ch & 0xf];
	}
	buf->size = (size_t)(p - buf->base);
	assert(buf->size == new_size);
	/* if no characters were written, it's the empty string,
	 * meaning the dns root zone.
	 */
	if (len == 0) {
		buf->base = realloc(buf->base, buf->size + 1);
		buf->base[buf->size++] = '.';
	}
}
