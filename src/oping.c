/**
 * Object oriented C module to send ICMP and ICMPv6 `echo's.
 * Copyright (C) 2006-2014  Florian octo Forster <ff at octo.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; only version 2 of the License is
 * applicable.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#if STDC_HEADERS
# include <stdlib.h>
# include <stdio.h>
# include <string.h>
# include <stdint.h>
# include <inttypes.h>
# include <errno.h>
# include <assert.h>
#else
# error "You don't have the standard C99 header files installed"
#endif /* STDC_HEADERS */

#if HAVE_UNISTD_H
# include <unistd.h>
#endif

#if HAVE_MATH_H
# include <math.h>
#endif

#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#if HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif
#if HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif
#if HAVE_NETINET_IP_H
# include <netinet/ip.h>
#endif

#if HAVE_NETDB_H
# include <netdb.h> /* NI_MAXHOST */
#endif

#if HAVE_SIGNAL_H
# include <signal.h>
#endif

#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include <locale.h>
#include <langinfo.h>

#if USE_NCURSES
# define NCURSES_OPAQUE 1
/* http://newsgroups.derkeiler.com/Archive/Rec/rec.games.roguelike.development/2010-09/msg00050.html */
# define _X_OPEN_SOURCE_EXTENDED

# if HAVE_NCURSESW_NCURSES_H
#  include <ncursesw/ncurses.h>
# elif HAVE_NCURSES_H
#  include <ncurses.h>
# endif

# define OPING_GREEN 1
# define OPING_YELLOW 2
# define OPING_RED 3
# define OPING_GREEN_HIST 4
# define OPING_YELLOW_HIST 5
# define OPING_RED_HIST 6

static char const * const hist_symbols_utf8[] = {
	"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
static size_t const hist_symbols_utf8_num = sizeof (hist_symbols_utf8)
	/ sizeof (hist_symbols_utf8[0]);

/* scancodes for 6 levels of horizontal bars, ncurses-specific */
/* those are not the usual constants because those are not constant */
static int const hist_symbols_acs[] = {
	115, /* ACS_S9 "⎽" */
	114, /* ACS_S7 "⎼" */
	113, /* ACS_S5 "─" */
	112, /* ACS_S3 "⎻" */
	111  /* ACS_S1 "⎺" */
};
static size_t const hist_symbols_acs_num = sizeof (hist_symbols_acs)
	/ sizeof (hist_symbols_acs[0]);

/* use different colors without a background for scancodes */
static int const hist_colors_utf8[] = {
	OPING_GREEN_HIST, OPING_YELLOW_HIST, OPING_RED_HIST };
static int const hist_colors_acs[] = {
	OPING_GREEN, OPING_YELLOW, OPING_RED };
/* assuming that both arrays are the same size */
static size_t const hist_colors_num = sizeof (hist_colors_utf8)
	/ sizeof (hist_colors_utf8[0]);
#endif

/* "─" */
#define BOXPLOT_WHISKER_BAR (113 | A_ALTCHARSET)
/* "├" */
#define BOXPLOT_WHISKER_LEFT_END (116 | A_ALTCHARSET)
/* "┤" */
#define BOXPLOT_WHISKER_RIGHT_END (117 | A_ALTCHARSET)
#define BOXPLOT_BOX ' '
/* "│" */
#define BOXPLOT_MEDIAN (120 | A_ALTCHARSET)

#include "oping.h"

#ifndef _POSIX_SAVED_IDS
# define _POSIX_SAVED_IDS 0
#endif

#ifndef IPTOS_MINCOST
# define IPTOS_MINCOST 0x02
#endif

/* Remove GNU specific __attribute__ settings when using another compiler */
#if !__GNUC__
# define __attribute__(x) /**/
#endif

typedef struct ping_context
{
	char host[NI_MAXHOST];
	char addr[NI_MAXHOST];

	int index;
	int req_sent;
	int req_rcvd;

	double latency_min;
	double latency_max;
	double latency_total;
	double latency_total_square;

/* 1000 + one "infinity" bucket. */
#define OPING_HISTOGRAM_BUCKETS 1001
	uint32_t *latency_histogram;
	size_t latency_histogram_size;

#if USE_NCURSES
	WINDOW *window;
#endif
} ping_context_t;

static double  opt_interval   = 1.0;
static int     opt_addrfamily = PING_DEF_AF;
static char   *opt_srcaddr    = NULL;
static char   *opt_device     = NULL;
static char   *opt_filename   = NULL;
static int     opt_count      = -1;
static int     opt_send_ttl   = 64;
static uint8_t opt_send_qos   = 0;
#define OPING_DEFAULT_PERCENTILE 95.0
static double  opt_percentile = -1.0;
static double  opt_exit_status_threshold = 1.0;
#if USE_NCURSES
static int     opt_utf8       = 0;
#endif

static int host_num = 0;

#if USE_NCURSES
static WINDOW *main_win = NULL;
#endif

static void sigint_handler (int signal) /* {{{ */
{
	/* Make compiler happy */
	signal = 0;
	/* Exit the loop */
	opt_count = 0;
} /* }}} void sigint_handler */

static ping_context_t *context_create (void) /* {{{ */
{
	ping_context_t *ret;

	if ((ret = malloc (sizeof (ping_context_t))) == NULL)
		return (NULL);

	memset (ret, '\0', sizeof (ping_context_t));

	ret->latency_min   = -1.0;
	ret->latency_max   = -1.0;
	ret->latency_total = 0.0;
	ret->latency_total_square = 0.0;

	ret->latency_histogram_size = (size_t) OPING_HISTOGRAM_BUCKETS;
	ret->latency_histogram = calloc (ret->latency_histogram_size,
			sizeof (*ret->latency_histogram));

#if USE_NCURSES
	ret->window = NULL;
#endif

	return (ret);
} /* }}} ping_context_t *context_create */

static void context_destroy (ping_context_t *context) /* {{{ */
{
	if (context == NULL)
		return;

#if USE_NCURSES
	if (context->window != NULL)
	{
		delwin (context->window);
		context->window = NULL;
	}
#endif

	free (context->latency_histogram);
	context->latency_histogram = NULL;

	free (context);
} /* }}} void context_destroy */

static double context_get_average (ping_context_t *ctx) /* {{{ */
{
	double num_total;

	if (ctx == NULL)
		return (-1.0);

	if (ctx->req_rcvd < 1)
		return (-0.0);

	num_total = (double) ctx->req_rcvd;
	return (ctx->latency_total / num_total);
} /* }}} double context_get_average */

static double context_get_percentile (ping_context_t *ctx, /* {{{ */
		double percentile)
{
	double threshold = percentile / 100.0;
	uint32_t accumulated[ctx->latency_histogram_size];
	double ratios[ctx->latency_histogram_size];
	double index_to_ms_factor;
	uint32_t num;
	size_t i;

	if (ctx->latency_histogram == NULL)
		return (NAN);

	accumulated[0] = ctx->latency_histogram[0];
	for (i = 1; i < ctx->latency_histogram_size; i++)
		accumulated[i] = accumulated[i - 1]
			+ ctx->latency_histogram[i];
	num = accumulated[ctx->latency_histogram_size - 1];

	for (i = 0; i < ctx->latency_histogram_size; i++)
	{
		ratios[i] = ((double) accumulated[i]) / ((double) num);
		if (ratios[i] >= threshold)
			break;
	}

	if (i >= ctx->latency_histogram_size)
		return (NAN);
	else if (i == (ctx->latency_histogram_size - 1))
		return (INFINITY);

	index_to_ms_factor = (1000.0 * opt_interval) / (ctx->latency_histogram_size - 1);

	/* Multiply with i+1, because we're interested in the _upper_ bound of
	 * each bucket. */
	return (index_to_ms_factor * ((double) (i + 1)));
} /* }}} double context_get_percentile */

static double context_get_stddev (ping_context_t *ctx) /* {{{ */
{
	double num_total;

	if (ctx == NULL)
		return (-1.0);

	if (ctx->req_rcvd < 1)
		return (-0.0);
	else if (ctx->req_rcvd < 2)
		return (0.0);

	num_total = (double) ctx->req_rcvd;
	return (sqrt (((num_total * ctx->latency_total_square)
					- (ctx->latency_total * ctx->latency_total))
				/ (num_total * (num_total - 1.0))));
} /* }}} double context_get_stddev */

static double context_get_packet_loss (const ping_context_t *ctx) /* {{{ */
{
	if (ctx == NULL)
		return (-1.0);

	if (ctx->req_sent < 1)
		return (0.0);

	return (100.0 * (ctx->req_sent - ctx->req_rcvd)
			/ ((double) ctx->req_sent));
} /* }}} double context_get_packet_loss */

static int ping_initialize_contexts (pingobj_t *ping) /* {{{ */
{
	pingobj_iter_t *iter;
	int index;

	if (ping == NULL)
		return (EINVAL);

	index = 0;
	for (iter = ping_iterator_get (ping);
			iter != NULL;
			iter = ping_iterator_next (iter))
	{
		ping_context_t *context;
		size_t buffer_size;

		context = context_create ();
		context->index = index;

		buffer_size = sizeof (context->host);
		ping_iterator_get_info (iter, PING_INFO_HOSTNAME, context->host, &buffer_size);

		buffer_size = sizeof (context->addr);
		ping_iterator_get_info (iter, PING_INFO_ADDRESS, context->addr, &buffer_size);

		ping_iterator_set_context (iter, (void *) context);

		index++;
	}

	return (0);
} /* }}} int ping_initialize_contexts */

static void usage_exit (const char *name, int status) /* {{{ */
{
	fprintf (stderr, "Usage: %s [OPTIONS] "
				"-f filename | host [host [host ...]]\n"

			"\nAvailable options:\n"
			"  -4|-6        force the use of IPv4 or IPv6\n"
			"  -c count     number of ICMP packets to send\n"
			"  -i interval  interval with which to send ICMP packets\n"
			"  -t ttl       time to live for each ICMP packet\n"
			"  -Q qos       Quality of Service (QoS) of outgoing packets\n"
			"               Use \"-Q help\" for a list of valid options.\n"
			"  -I srcaddr   source address\n"
			"  -D device    outgoing interface name\n"
			"  -f filename  filename to read hosts from\n"
#if USE_NCURSES
			"  -u / -U      force / disable UTF-8 output\n"
#endif
			"  -P percent   Report the n'th percentile of latency\n"
			"  -Z percent   Exit with non-zero exit status if more than this percentage of\n"
			"               probes timed out. (default: never)\n"

			"\noping "PACKAGE_VERSION", http://verplant.org/liboping/\n"
			"by Florian octo Forster <octo@verplant.org>\n"
			"for contributions see `AUTHORS'\n",
			name);
	exit (status);
} /* }}} void usage_exit */

__attribute__((noreturn))
static void usage_qos_exit (const char *arg, int status) /* {{{ */
{
	if (arg != 0)
		fprintf (stderr, "Invalid QoS argument: \"%s\"\n\n", arg);

	fprintf (stderr, "Valid QoS arguments (option \"-Q\") are:\n"
			"\n"
			"  Differentiated Services (IPv4 and IPv6, RFC 2474)\n"
			"\n"
			"    be                     Best Effort (BE, default PHB).\n"
			"    ef                     Expedited Forwarding (EF) PHB group (RFC 3246).\n"
			"                           (low delay, low loss, low jitter)\n"
			"    va                     Voice Admit (VA) DSCP (RFC 5865).\n"
			"                           (capacity-admitted traffic)\n"
			"    af[1-4][1-3]           Assured Forwarding (AF) PHB group (RFC 2597).\n"
			"                           For example: \"af12\" (class 1, precedence 2)\n"
			"    cs[0-7]                Class Selector (CS) PHB group (RFC 2474).\n"
			"                           For example: \"cs1\" (priority traffic)\n"
			"\n"
			"  Type of Service (IPv4, RFC 1349, obsolete)\n"
			"\n"
			"    lowdelay     (%#04x)    minimize delay\n"
			"    throughput   (%#04x)    maximize throughput\n"
			"    reliability  (%#04x)    maximize reliability\n"
			"    mincost      (%#04x)    minimize monetary cost\n"
			"\n"
			"  Specify manually\n"
			"\n"
			"    0x00 - 0xff            Hexadecimal numeric specification.\n"
			"       0 -  255            Decimal numeric specification.\n"
			"\n",
			(unsigned int) IPTOS_LOWDELAY,
			(unsigned int) IPTOS_THROUGHPUT,
			(unsigned int) IPTOS_RELIABILITY,
			(unsigned int) IPTOS_MINCOST);

	exit (status);
} /* }}} void usage_qos_exit */

static int set_opt_send_qos (const char *opt) /* {{{ */
{
	if (opt == NULL)
		return (EINVAL);

	if (strcasecmp ("help", opt) == 0)
		usage_qos_exit (/* arg = */ NULL, /* status = */ EXIT_SUCCESS);
	/* DiffServ (RFC 2474): */
	/* - Best effort (BE) */
	else if (strcasecmp ("be", opt) == 0)
		opt_send_qos = 0;
	/* - Expedited Forwarding (EF, RFC 3246) */
	else if (strcasecmp ("ef", opt) == 0)
		opt_send_qos = 0xB8; /* == 0x2E << 2 */
	/* - Voice Admit (VA, RFC 5865) */
	else if (strcasecmp ("va", opt) == 0)
		opt_send_qos = 0xB0; /* == 0x2D << 2 */
	/* - Assured Forwarding (AF, RFC 2597) */
	else if ((strncasecmp ("af", opt, strlen ("af")) == 0)
			&& (strlen (opt) == 4))
	{
		uint8_t dscp;
		uint8_t class = 0;
		uint8_t prec = 0;

		/* There are four classes, AF1x, AF2x, AF3x, and AF4x. */
		if (opt[2] == '1')
			class = 1;
		else if (opt[2] == '2')
			class = 2;
		else if (opt[2] == '3')
			class = 3;
		else if (opt[2] == '4')
			class = 4;
		else
			usage_qos_exit (/* arg = */ opt, /* status = */ EXIT_SUCCESS);

		/* In each class, there are three precedences, AFx1, AFx2, and AFx3 */
		if (opt[3] == '1')
			prec = 1;
		else if (opt[3] == '2')
			prec = 2;
		else if (opt[3] == '3')
			prec = 3;
		else
			usage_qos_exit (/* arg = */ opt, /* status = */ EXIT_SUCCESS);

		dscp = (8 * class) + (2 * prec);
		/* The lower two bits are used for Explicit Congestion Notification (ECN) */
		opt_send_qos = dscp << 2;
	}
	/* - Class Selector (CS) */
	else if ((strncasecmp ("cs", opt, strlen ("cs")) == 0)
			&& (strlen (opt) == 3))
	{
		uint8_t class;

		if ((opt[2] < '0') || (opt[2] > '7'))
			usage_qos_exit (/* arg = */ opt, /* status = */ EXIT_FAILURE);

		/* Not exactly legal by the C standard, but I don't know of any
		 * system not supporting this hack. */
		class = ((uint8_t) opt[2]) - ((uint8_t) '0');
		opt_send_qos = class << 5;
	}
	/* Type of Service (RFC 1349) */
	else if (strcasecmp ("lowdelay", opt) == 0)
		opt_send_qos = IPTOS_LOWDELAY;
	else if (strcasecmp ("throughput", opt) == 0)
		opt_send_qos = IPTOS_THROUGHPUT;
	else if (strcasecmp ("reliability", opt) == 0)
		opt_send_qos = IPTOS_RELIABILITY;
	else if (strcasecmp ("mincost", opt) == 0)
		opt_send_qos = IPTOS_MINCOST;
	/* Numeric value */
	else
	{
		unsigned long value;
		char *endptr;

		errno = 0;
		endptr = NULL;
		value = strtoul (opt, &endptr, /* base = */ 0);
		if ((errno != 0) || (endptr == opt)
				|| (endptr == NULL) || (*endptr != 0)
				|| (value > 0xff))
			usage_qos_exit (/* arg = */ opt, /* status = */ EXIT_FAILURE);
		
		opt_send_qos = (uint8_t) value;
	}

	return (0);
} /* }}} int set_opt_send_qos */

static char *format_qos (uint8_t qos, char *buffer, size_t buffer_size) /* {{{ */
{
	uint8_t dscp;
	uint8_t ecn;
	char *dscp_str;
	char *ecn_str;

	dscp = qos >> 2;
	ecn = qos & 0x03;

	switch (dscp)
	{
		case 0x00: dscp_str = "be";  break;
		case 0x2e: dscp_str = "ef";  break;
		case 0x2d: dscp_str = "va";  break;
		case 0x0a: dscp_str = "af11"; break;
		case 0x0c: dscp_str = "af12"; break;
		case 0x0e: dscp_str = "af13"; break;
		case 0x12: dscp_str = "af21"; break;
		case 0x14: dscp_str = "af22"; break;
		case 0x16: dscp_str = "af23"; break;
		case 0x1a: dscp_str = "af31"; break;
		case 0x1c: dscp_str = "af32"; break;
		case 0x1e: dscp_str = "af33"; break;
		case 0x22: dscp_str = "af41"; break;
		case 0x24: dscp_str = "af42"; break;
		case 0x26: dscp_str = "af43"; break;
		case 0x08: dscp_str = "cs1";  break;
		case 0x10: dscp_str = "cs2";  break;
		case 0x18: dscp_str = "cs3";  break;
		case 0x20: dscp_str = "cs4";  break;
		case 0x28: dscp_str = "cs5";  break;
		case 0x30: dscp_str = "cs6";  break;
		case 0x38: dscp_str = "cs7";  break;
		default:   dscp_str = NULL;
	}

	switch (ecn)
	{
		case 0x01: ecn_str = ",ecn(1)"; break;
		case 0x02: ecn_str = ",ecn(0)"; break;
		case 0x03: ecn_str = ",ce"; break;
		default:   ecn_str = "";
	}

	if (dscp_str == NULL)
		snprintf (buffer, buffer_size, "0x%02x%s", dscp, ecn_str);
	else
		snprintf (buffer, buffer_size, "%s%s", dscp_str, ecn_str);
	buffer[buffer_size - 1] = 0;

	return (buffer);
} /* }}} char *format_qos */

static int read_options (int argc, char **argv) /* {{{ */
{
	int optchar;

	while (1)
	{
		optchar = getopt (argc, argv, "46c:hi:I:t:Q:f:D:Z:P:"
#if USE_NCURSES
				"uU"
#endif
				);

		if (optchar == -1)
			break;

		switch (optchar)
		{
			case '4':
			case '6':
				opt_addrfamily = (optchar == '4') ? AF_INET : AF_INET6;
				break;

			case 'c':
				{
					int new_count;
					new_count = atoi (optarg);
					if (new_count > 0)
					{
						opt_count = new_count;

						if ((opt_percentile < 0.0) && (opt_count < 20))
							opt_percentile = 100.0 * (opt_count - 1) / opt_count;
					}
					else
						fprintf(stderr, "Ignoring invalid count: %s\n",
								optarg);
				}
				break;

			case 'f':
				{
					if (opt_filename != NULL)
						free (opt_filename);
					opt_filename = strdup (optarg);
				}
				break;

			case 'i':
				{
					double new_interval;
					new_interval = atof (optarg);
					if (new_interval < 0.001)
						fprintf (stderr, "Ignoring invalid interval: %s\n",
								optarg);
					else
						opt_interval = new_interval;
				}
				break;

			case 'I':
				{
					if (opt_srcaddr != NULL)
						free (opt_srcaddr);
					opt_srcaddr = strdup (optarg);
				}
				break;

			case 'D':
				opt_device = optarg;
				break;

			case 't':
			{
				int new_send_ttl;
				new_send_ttl = atoi (optarg);
				if ((new_send_ttl > 0) && (new_send_ttl < 256))
					opt_send_ttl = new_send_ttl;
				else
					fprintf (stderr, "Ignoring invalid TTL argument: %s\n",
							optarg);
				break;
			}

			case 'Q':
				set_opt_send_qos (optarg);
				break;

			case 'P':
				{
					double new_percentile;
					new_percentile = atof (optarg);
					if (isnan (new_percentile)
							|| (new_percentile < 0.1)
							|| (new_percentile > 100.0))
						fprintf (stderr, "Ignoring invalid percentile: %s\n",
								optarg);
					else
						opt_percentile = new_percentile;
				}
				break;

#if USE_NCURSES
			case 'u':
				opt_utf8 = 2;
				break;
			case 'U':
				opt_utf8 = 1;
				break;
#endif

			case 'Z':
			{
				char *endptr = NULL;
				double tmp;

				errno = 0;
				tmp = strtod (optarg, &endptr);
				if ((errno != 0) || (endptr == NULL) || (*endptr != 0) || (tmp < 0.0) || (tmp > 100.0))
				{
					fprintf (stderr, "Ignoring invalid -Z argument: %s\n", optarg);
					fprintf (stderr, "The \"-Z\" option requires a numeric argument between 0 and 100.\n");
				}
				else
					opt_exit_status_threshold = tmp / 100.0;

				break;
			}

			case 'h':
				usage_exit (argv[0], 0);
				break;
			default:
				usage_exit (argv[0], 1);
		}
	}

	if (opt_percentile <= 0.0)
		opt_percentile = OPING_DEFAULT_PERCENTILE;

	return (optind);
} /* }}} read_options */

static void time_normalize (struct timespec *ts) /* {{{ */
{
	while (ts->tv_nsec < 0)
	{
		if (ts->tv_sec == 0)
		{
			ts->tv_nsec = 0;
			return;
		}

		ts->tv_sec  -= 1;
		ts->tv_nsec += 1000000000;
	}

	while (ts->tv_nsec >= 1000000000)
	{
		ts->tv_sec  += 1;
		ts->tv_nsec -= 1000000000;
	}
} /* }}} void time_normalize */

static void time_calc (struct timespec *ts_dest, /* {{{ */
		const struct timespec *ts_int,
		const struct timeval  *tv_begin,
		const struct timeval  *tv_end)
{
	ts_dest->tv_sec = tv_begin->tv_sec + ts_int->tv_sec;
	ts_dest->tv_nsec = (tv_begin->tv_usec * 1000) + ts_int->tv_nsec;
	time_normalize (ts_dest);

	/* Assure that `(begin + interval) > end'.
	 * This may seem overly complicated, but `tv_sec' is of type `time_t'
	 * which may be `unsigned. *sigh* */
	if ((tv_end->tv_sec > ts_dest->tv_sec)
			|| ((tv_end->tv_sec == ts_dest->tv_sec)
				&& ((tv_end->tv_usec * 1000) > ts_dest->tv_nsec)))
	{
		ts_dest->tv_sec  = 0;
		ts_dest->tv_nsec = 0;
		return;
	}

	ts_dest->tv_sec = ts_dest->tv_sec - tv_end->tv_sec;
	ts_dest->tv_nsec = ts_dest->tv_nsec - (tv_end->tv_usec * 1000);
	time_normalize (ts_dest);
} /* }}} void time_calc */

#if USE_NCURSES
static _Bool has_utf8() /* {{{ */
{
# if HAVE_NCURSESW_NCURSES_H
	if (!opt_utf8)
	{
		/* Automatically determine */
		if (strcasecmp ("UTF-8", nl_langinfo (CODESET)) == 0)
			opt_utf8 = 2;
		else
			opt_utf8 = 1;
	}
	return ((_Bool) (opt_utf8 - 1));
# else
	return (0);
# endif
} /* }}} _Bool has_utf8 */

static int update_boxplot (ping_context_t *ctx) /* {{{ */
{
	uint32_t *accumulated;
	double *ratios;
	uint32_t num;
	size_t i;
	size_t x_max;
	size_t x;

	x_max = (size_t) getmaxx (ctx->window);
	if (x_max <= 4)
		return (EINVAL);
	x_max -= 4;

	accumulated = calloc (x_max, sizeof (*accumulated));
	ratios = calloc (x_max, sizeof (*ratios));

	/* Downsample */
	for (i = 0; i < ctx->latency_histogram_size; i++)
	{
		x = i * x_max / ctx->latency_histogram_size;
		accumulated[x] += ctx->latency_histogram[i];
	}

	/* Sum */
	for (x = 1; x < x_max; x++)
		accumulated[x] += accumulated[x - 1];

	num = accumulated[x_max - 1];

	/* Calculate ratios */
	for (x = 0; x < x_max; x++)
		ratios[x] = ((double) accumulated[x]) / ((double) num);

	for (x = 0; x < x_max; x++)
	{
		int symbol = ' ';
		_Bool reverse = 0;

		if (x == 0)
		{
			if (ratios[x] >= 0.5)
			{
				symbol = BOXPLOT_MEDIAN;
				reverse = 1;
			}
			else if (ratios[x] > 0.25)
			{
				symbol = BOXPLOT_BOX;
				reverse = 1;
			}
			else if (ratios[x] > 0.025)
				symbol = BOXPLOT_WHISKER_BAR;
			else
				symbol = ' '; /* NOP */
		}
		else /* (x != 0) */
		{
			if ((ratios[x - 1] < 0.5) && (ratios[x] >= 0.5))
			{
				symbol = BOXPLOT_MEDIAN;
				reverse = 1;
			}
			else if (((ratios[x] >= 0.25) && (ratios[x] <= 0.75))
					|| ((ratios[x - 1] < 0.75) && (ratios[x] > 0.75)))
			{
				symbol = BOXPLOT_BOX;
				reverse = 1;
			}
			else if ((ratios[x] < 0.5) && (ratios[x] >= 0.025))
			{
				if (ratios[x - 1] < 0.025)
					symbol = BOXPLOT_WHISKER_LEFT_END;
				else
					symbol = BOXPLOT_WHISKER_BAR;
			}
			else if ((ratios[x] > .5) && (ratios[x] < 0.975))
			{
				symbol = BOXPLOT_WHISKER_BAR;
			}
			else if ((ratios[x] >= 0.975) && (ratios[x - 1] < 0.975))
				symbol = BOXPLOT_WHISKER_RIGHT_END;
		}

		if (reverse)
			wattron (ctx->window, A_REVERSE);
		mvwaddch (ctx->window, /* y = */ 3, /* x = */ (int) (x + 2), symbol);
		// mvwprintw (ctx->window, /* y = */ 3, /* x = */ (int) (x + 2), symbol);
		if (reverse)
			wattroff (ctx->window, A_REVERSE);
	}

	free (ratios);
	free (accumulated);
	return (0);
} /* }}} int update_boxplot */

static int update_prettyping_graph (ping_context_t *ctx, /* {{{ */
		double latency, unsigned int sequence)
{
	int color = OPING_RED;
	char const *symbol = "!";
	int symbolc = '!';

	int x_max;
	int x_pos;

	x_max = getmaxx (ctx->window);
	x_pos = ((sequence - 1) % (x_max - 4)) + 2;

	if (latency >= 0.0)
	{
		double ratio;

		size_t symbols_num = hist_symbols_acs_num;
		size_t colors_num = 1;

		size_t index_symbols;
		size_t index_colors;
		size_t intensity;

		/* latency is in milliseconds, opt_interval is in seconds. */
		ratio = (latency * 0.001) / opt_interval;
		if (ratio > 1) {
			ratio = 1.0;
		}

		if (has_utf8 ())
			symbols_num = hist_symbols_utf8_num;

		if (has_colors () == TRUE)
			colors_num = hist_colors_num;

		intensity = (size_t) (ratio * ((double) (symbols_num * colors_num)));
		if (intensity >= (symbols_num * colors_num))
			intensity = (symbols_num * colors_num) - 1;

		index_symbols = intensity % symbols_num;
		assert (index_symbols < symbols_num);

		index_colors = intensity / symbols_num;
		assert (index_colors < colors_num);

		if (has_utf8())
		{
			color = hist_colors_utf8[index_colors];
			symbol = hist_symbols_utf8[index_symbols];
		}
		else
		{
			color = hist_colors_acs[index_colors];
			symbolc = hist_symbols_acs[index_symbols] | A_ALTCHARSET;
		}
	}
	else /* if (!(latency >= 0.0)) */
		wattron (ctx->window, A_BOLD);

	if (has_colors () == TRUE)
		wattron (ctx->window, COLOR_PAIR(color));

	if (has_utf8())
		mvwprintw (ctx->window, /* y = */ 3, /* x = */ x_pos, symbol);
	else
		mvwaddch (ctx->window, /* y = */ 3, /* x = */ x_pos, symbolc);

	if (has_colors () == TRUE)
		wattroff (ctx->window, COLOR_PAIR(color));

	/* Use negation here to handle NaN correctly. */
	if (!(latency >= 0.0))
		wattroff (ctx->window, A_BOLD);

	wprintw (ctx->window, " ");
	return (0);
} /* }}} int update_prettyping_graph */

static int update_stats_from_context (ping_context_t *ctx, pingobj_iter_t *iter) /* {{{ */
{
	double latency = -1.0;
	size_t buffer_len = sizeof (latency);

	ping_iterator_get_info (iter, PING_INFO_LATENCY,
			&latency, &buffer_len);

	unsigned int sequence = 0;
	buffer_len = sizeof (sequence);
	ping_iterator_get_info (iter, PING_INFO_SEQUENCE,
			&sequence, &buffer_len);


	if ((ctx == NULL) || (ctx->window == NULL))
		return (EINVAL);

	/* werase (ctx->window); */

	box (ctx->window, 0, 0);
	wattron (ctx->window, A_BOLD);
	mvwprintw (ctx->window, /* y = */ 0, /* x = */ 5,
			" %s ", ctx->host);
	wattroff (ctx->window, A_BOLD);
	wprintw (ctx->window, "ping statistics ");
	mvwprintw (ctx->window, /* y = */ 1, /* x = */ 2,
			"%i packets transmitted, %i received, %.2f%% packet "
			"loss, time %.1fms",
			ctx->req_sent, ctx->req_rcvd,
			context_get_packet_loss (ctx),
			ctx->latency_total);
	if (ctx->req_rcvd != 0)
	{
		double average;
		double deviation;
		double percentile;

		average = context_get_average (ctx);
		deviation = context_get_stddev (ctx);
		percentile = context_get_percentile (ctx, opt_percentile);

		mvwprintw (ctx->window, /* y = */ 2, /* x = */ 2,
				"rtt min/avg/%.0f%%/max/sdev = "
				"%.3f/%.3f/%.0f/%.3f/%.3f ms\n",
				opt_percentile,
				ctx->latency_min,
				average,
				percentile,
				ctx->latency_max,
				deviation);
	}

	update_prettyping_graph (ctx, latency, sequence);

	wrefresh (ctx->window);

	return (0);
} /* }}} int update_stats_from_context */

static int on_resize (pingobj_t *ping) /* {{{ */
{
	pingobj_iter_t *iter;
	int width = 0;
	int height = 0;
	int main_win_height;

	getmaxyx (stdscr, height, width);
	if ((height < 1) || (width < 1))
		return (EINVAL);

	main_win_height = height - (5 * host_num);
	wresize (main_win, main_win_height, /* width = */ width);
	/* Allow scrolling */
	scrollok (main_win, TRUE);
	/* wsetscrreg (main_win, 0, main_win_height - 1); */
	/* Allow hardware accelerated scrolling. */
	idlok (main_win, TRUE);
	wrefresh (main_win);

	for (iter = ping_iterator_get (ping);
			iter != NULL;
			iter = ping_iterator_next (iter))
	{
		ping_context_t *context;

		context = ping_iterator_get_context (iter);
		if (context == NULL)
			continue;

		if (context->window != NULL)
		{
			delwin (context->window);
			context->window = NULL;
		}
		context->window = newwin (/* height = */ 5,
				/* width = */ width,
				/* y = */ main_win_height + (5 * context->index),
				/* x = */ 0);
	}

	return (0);
} /* }}} */

static int check_resize (pingobj_t *ping) /* {{{ */
{
	int need_resize = 0;

	while (42)
	{
		int key = wgetch (stdscr);
		if (key == ERR)
			break;
		else if (key == KEY_RESIZE)
			need_resize = 1;
	}

	if (need_resize)
		return (on_resize (ping));
	else
		return (0);
} /* }}} int check_resize */

static int pre_loop_hook (pingobj_t *ping) /* {{{ */
{
	pingobj_iter_t *iter;
	int width = 0;
	int height = 0;
	int main_win_height;

	initscr ();
	cbreak ();
	noecho ();
	nodelay (stdscr, TRUE);

	getmaxyx (stdscr, height, width);
	if ((height < 1) || (width < 1))
		return (EINVAL);

	if (has_colors () == TRUE)
	{
		start_color ();
		init_pair (OPING_GREEN,  COLOR_GREEN,  /* default = */ 0);
		init_pair (OPING_YELLOW, COLOR_YELLOW, /* default = */ 0);
		init_pair (OPING_RED,    COLOR_RED,    /* default = */ 0);
		init_pair (OPING_GREEN_HIST,  COLOR_GREEN,  COLOR_BLACK);
		init_pair (OPING_YELLOW_HIST, COLOR_YELLOW, COLOR_GREEN);
		init_pair (OPING_RED_HIST,    COLOR_RED,    COLOR_YELLOW);
	}

	main_win_height = height - (5 * host_num);
	main_win = newwin (/* height = */ main_win_height,
			/* width = */ width,
			/* y = */ 0, /* x = */ 0);
	/* Allow scrolling */
	scrollok (main_win, TRUE);
	/* wsetscrreg (main_win, 0, main_win_height - 1); */
	/* Allow hardware accelerated scrolling. */
	idlok (main_win, TRUE);
	wmove (main_win, /* y = */ main_win_height - 1, /* x = */ 0);
	wrefresh (main_win);

	for (iter = ping_iterator_get (ping);
			iter != NULL;
			iter = ping_iterator_next (iter))
	{
		ping_context_t *context;

		context = ping_iterator_get_context (iter);
		if (context == NULL)
			continue;

		if (context->window != NULL)
		{
			delwin (context->window);
			context->window = NULL;
		}
		context->window = newwin (/* height = */ 5,
				/* width = */ width,
				/* y = */ main_win_height + (5 * context->index),
				/* x = */ 0);
	}


	/* Don't know what good this does exactly, but without this code
	 * "check_resize" will be called right after startup and *somehow*
	 * this leads to display errors. If we purge all initial characters
	 * here, the problem goes away. "wgetch" is non-blocking due to
	 * "nodelay" (see above). */
	while (wgetch (stdscr) != ERR)
	{
		/* eat up characters */;
	}

	return (0);
} /* }}} int pre_loop_hook */

static int pre_sleep_hook (pingobj_t *ping) /* {{{ */
{
	return (check_resize (ping));
} /* }}} int pre_sleep_hook */

static int post_sleep_hook (pingobj_t *ping) /* {{{ */
{
	return (check_resize (ping));
} /* }}} int pre_sleep_hook */
#else /* if !USE_NCURSES */
static int pre_loop_hook (pingobj_t *ping) /* {{{ */
{
	pingobj_iter_t *iter;

	for (iter = ping_iterator_get (ping);
			iter != NULL;
			iter = ping_iterator_next (iter))
	{
		ping_context_t *ctx;
		size_t buffer_size;

		ctx = ping_iterator_get_context (iter);
		if (ctx == NULL)
			continue;

		buffer_size = 0;
		ping_iterator_get_info (iter, PING_INFO_DATA, NULL, &buffer_size);

		printf ("PING %s (%s) %zu bytes of data.\n",
				ctx->host, ctx->addr, buffer_size);
	}

	return (0);
} /* }}} int pre_loop_hook */

static int pre_sleep_hook (__attribute__((unused)) pingobj_t *ping) /* {{{ */
{
	fflush (stdout);

	return (0);
} /* }}} int pre_sleep_hook */

static int post_sleep_hook (__attribute__((unused)) pingobj_t *ping) /* {{{ */
{
	return (0);
} /* }}} int post_sleep_hook */
#endif

static void update_context (ping_context_t *context, double latency) /* {{{ */
{
	size_t bucket;

	context->req_rcvd++;
	context->latency_total += latency;
	context->latency_total_square += (latency * latency);

	if ((context->latency_max < 0.0) || (context->latency_max < latency))
		context->latency_max = latency;
	if ((context->latency_min < 0.0) || (context->latency_min > latency))
		context->latency_min = latency;

	if (context->latency_histogram == NULL)
		return;

	/* latency is in ms, opt_interval is in s. */
	bucket = (size_t) ((latency * (context->latency_histogram_size - 1))
			/ (1000.0 * opt_interval));
	if (bucket >= context->latency_histogram_size)
		bucket = context->latency_histogram_size - 1;
	context->latency_histogram[bucket]++;
} /* }}} void update_context */

static void update_host_hook (pingobj_iter_t *iter, /* {{{ */
		__attribute__((unused)) int index)
{
	double          latency;
	unsigned int    sequence;
	int             recv_ttl;
	uint8_t         recv_qos;
	char            recv_qos_str[16];
	size_t          buffer_len;
	size_t          data_len;
	ping_context_t *context;

	latency = -1.0;
	buffer_len = sizeof (latency);
	ping_iterator_get_info (iter, PING_INFO_LATENCY,
			&latency, &buffer_len);

	sequence = 0;
	buffer_len = sizeof (sequence);
	ping_iterator_get_info (iter, PING_INFO_SEQUENCE,
			&sequence, &buffer_len);

	recv_ttl = -1;
	buffer_len = sizeof (recv_ttl);
	ping_iterator_get_info (iter, PING_INFO_RECV_TTL,
			&recv_ttl, &buffer_len);

	recv_qos = 0;
	buffer_len = sizeof (recv_qos);
	ping_iterator_get_info (iter, PING_INFO_RECV_QOS,
			&recv_qos, &buffer_len);

	data_len = 0;
	ping_iterator_get_info (iter, PING_INFO_DATA,
			NULL, &data_len);

	context = (ping_context_t *) ping_iterator_get_context (iter);

#if USE_NCURSES
# define HOST_PRINTF(...) wprintw(main_win, __VA_ARGS__)
#else
# define HOST_PRINTF(...) printf(__VA_ARGS__)
#endif

	context->req_sent++;
	if (latency > 0.0)
	{
		update_context (context, latency);

#if USE_NCURSES
		if (has_colors () == TRUE)
		{
			int color = OPING_GREEN;
			double average = context_get_average (context);
			double stddev = context_get_stddev (context);

			if ((latency < (average - (2 * stddev)))
					|| (latency > (average + (2 * stddev))))
				color = OPING_RED;
			else if ((latency < (average - stddev))
					|| (latency > (average + stddev)))
				color = OPING_YELLOW;

			HOST_PRINTF ("%zu bytes from %s (%s): icmp_seq=%u ttl=%i ",
					data_len, context->host, context->addr,
					sequence, recv_ttl,
					format_qos (recv_qos, recv_qos_str, sizeof (recv_qos_str)));
			if ((recv_qos != 0) || (opt_send_qos != 0))
			{
				HOST_PRINTF ("qos=%s ",
						format_qos (recv_qos, recv_qos_str, sizeof (recv_qos_str)));
			}
			HOST_PRINTF ("time=");
			wattron (main_win, COLOR_PAIR(color));
			HOST_PRINTF ("%.2f", latency);
			wattroff (main_win, COLOR_PAIR(color));
			HOST_PRINTF (" ms\n");
		}
		else
		{
#endif
		HOST_PRINTF ("%zu bytes from %s (%s): icmp_seq=%u ttl=%i ",
				data_len,
				context->host, context->addr,
				sequence, recv_ttl);
		if ((recv_qos != 0) || (opt_send_qos != 0))
		{
			HOST_PRINTF ("qos=%s ",
					format_qos (recv_qos, recv_qos_str, sizeof (recv_qos_str)));
		}
		HOST_PRINTF ("time=%.2f ms\n", latency);
#if USE_NCURSES
		}
#endif
	}
	else
	{
#if USE_NCURSES
		if (has_colors () == TRUE)
		{
			HOST_PRINTF ("echo reply from %s (%s): icmp_seq=%u ",
					context->host, context->addr,
					sequence);
			wattron (main_win, COLOR_PAIR(OPING_RED) | A_BOLD);
			HOST_PRINTF ("timeout");
			wattroff (main_win, COLOR_PAIR(OPING_RED) | A_BOLD);
			HOST_PRINTF ("\n");
		}
		else
		{
#endif
		HOST_PRINTF ("echo reply from %s (%s): icmp_seq=%u timeout\n",
				context->host, context->addr,
				sequence);
#if USE_NCURSES
		}
#endif
	}

#if USE_NCURSES
	update_stats_from_context (context, iter);
	wrefresh (main_win);
#endif
} /* }}} void update_host_hook */

/* Prints statistics for each host, cleans up the contexts and returns the
 * number of hosts which failed to return more than the fraction
 * opt_exit_status_threshold of pings. */
static int post_loop_hook (pingobj_t *ping) /* {{{ */
{
	pingobj_iter_t *iter;
	int failure_count = 0;

#if USE_NCURSES
	endwin ();
#endif

	for (iter = ping_iterator_get (ping);
			iter != NULL;
			iter = ping_iterator_next (iter))
	{
		ping_context_t *context;

		context = ping_iterator_get_context (iter);

		printf ("\n--- %s ping statistics ---\n"
				"%i packets transmitted, %i received, %.2f%% packet loss, time %.1fms\n",
				context->host, context->req_sent, context->req_rcvd,
				context_get_packet_loss (context),
				context->latency_total);

		{
			double pct_failed = 1.0 - (((double) context->req_rcvd)
					/ ((double) context->req_sent));
			if (pct_failed > opt_exit_status_threshold)
				failure_count++;
		}

		if (context->req_rcvd != 0)
		{
			double average;
			double deviation;
			double percentile;

			average = context_get_average (context);
			deviation = context_get_stddev (context);
			percentile = context_get_percentile (context, opt_percentile);

			printf ("rtt min/avg/%.0f%%/max/sdev = "
					"%.3f/%.3f/%.0f/%.3f/%.3f ms\n",
					opt_percentile,
					context->latency_min,
					average,
					percentile,
					context->latency_max,
					deviation);
		}

		ping_iterator_set_context (iter, NULL);
		context_destroy (context);
	}

	return (failure_count);
} /* }}} int post_loop_hook */

int main (int argc, char **argv) /* {{{ */
{
	pingobj_t      *ping;
	pingobj_iter_t *iter;

	struct sigaction sigint_action;

	struct timeval  tv_begin;
	struct timeval  tv_end;
	struct timespec ts_wait;
	struct timespec ts_int;

	int optind;
	int i;
	int status;
#if _POSIX_SAVED_IDS
	uid_t saved_set_uid;

	/* Save the old effective user id */
	saved_set_uid = geteuid ();
	/* Set the effective user ID to the real user ID without changing the
	 * saved set-user ID */
	status = seteuid (getuid ());
	if (status != 0)
	{
		fprintf (stderr, "Temporarily dropping privileges "
				"failed: %s\n", strerror (errno));
		exit (EXIT_FAILURE);
	}
#endif

        setlocale(LC_ALL, "");
	optind = read_options (argc, argv);

#if !_POSIX_SAVED_IDS
	/* Cannot temporarily drop privileges -> reject every file but "-". */
	if ((opt_filename != NULL)
			&& (strcmp ("-", opt_filename) != 0)
			&& (getuid () != geteuid ()))
	{
		fprintf (stderr, "Your real and effective user IDs don't "
				"match. Reading from a file (option '-f')\n"
				"is therefore too risky. You can still read "
				"from STDIN using '-f -' if you like.\n"
				"Sorry.\n");
		exit (EXIT_FAILURE);
	}
#endif

	if ((optind >= argc) && (opt_filename == NULL)) {
		usage_exit (argv[0], 1);
	}

	if ((ping = ping_construct ()) == NULL)
	{
		fprintf (stderr, "ping_construct failed\n");
		return (1);
	}

	if (ping_setopt (ping, PING_OPT_TTL, &opt_send_ttl) != 0)
	{
		fprintf (stderr, "Setting TTL to %i failed: %s\n",
				opt_send_ttl, ping_get_error (ping));
	}

	if (ping_setopt (ping, PING_OPT_QOS, &opt_send_qos) != 0)
	{
		fprintf (stderr, "Setting TOS to %i failed: %s\n",
				opt_send_qos, ping_get_error (ping));
	}

	{
		double temp_sec;
		double temp_nsec;

		temp_nsec = modf (opt_interval, &temp_sec);
		ts_int.tv_sec  = (time_t) temp_sec;
		ts_int.tv_nsec = (long) (temp_nsec * 1000000000L);

		/* printf ("ts_int = %i.%09li\n", (int) ts_int.tv_sec, ts_int.tv_nsec); */
	}

	if (opt_addrfamily != PING_DEF_AF)
		ping_setopt (ping, PING_OPT_AF, (void *) &opt_addrfamily);

	if (opt_srcaddr != NULL)
	{
		if (ping_setopt (ping, PING_OPT_SOURCE, (void *) opt_srcaddr) != 0)
		{
			fprintf (stderr, "Setting source address failed: %s\n",
					ping_get_error (ping));
		}
	}

	if (opt_device != NULL)
	{
		if (ping_setopt (ping, PING_OPT_DEVICE, (void *) opt_device) != 0)
		{
			fprintf (stderr, "Setting device failed: %s\n",
					ping_get_error (ping));
		}
	}

	if (opt_filename != NULL)
	{
		FILE *infile;
		char line[256];
		char host[256];

		if (strcmp (opt_filename, "-") == 0)
			/* Open STDIN */
			infile = fdopen(0, "r");
		else
			infile = fopen(opt_filename, "r");

		if (infile == NULL)
		{
			fprintf (stderr, "Opening %s failed: %s\n",
					(strcmp (opt_filename, "-") == 0)
					? "STDIN" : opt_filename,
					strerror(errno));
			return (1);
		}

#if _POSIX_SAVED_IDS
		/* Regain privileges */
		status = seteuid (saved_set_uid);
		if (status != 0)
		{
			fprintf (stderr, "Temporarily re-gaining privileges "
					"failed: %s\n", strerror (errno));
			exit (EXIT_FAILURE);
		}
#endif

		while (fgets(line, sizeof(line), infile))
		{
			/* Strip whitespace */
			if (sscanf(line, "%s", host) != 1)
				continue;

			if ((host[0] == 0) || (host[0] == '#'))
				continue;

			if (ping_host_add(ping, host) < 0)
			{
				const char *errmsg = ping_get_error (ping);

				fprintf (stderr, "Adding host `%s' failed: %s\n", host, errmsg);
				continue;
			}
			else
			{
				host_num++;
			}
		}

#if _POSIX_SAVED_IDS
		/* Drop privileges */
		status = seteuid (getuid ());
		if (status != 0)
		{
			fprintf (stderr, "Temporarily dropping privileges "
					"failed: %s\n", strerror (errno));
			exit (EXIT_FAILURE);
		}
#endif

		fclose(infile);
	}

#if _POSIX_SAVED_IDS
	/* Regain privileges */
	status = seteuid (saved_set_uid);
	if (status != 0)
	{
		fprintf (stderr, "Temporarily re-gaining privileges "
				"failed: %s\n", strerror (errno));
		exit (EXIT_FAILURE);
	}
#endif

	for (i = optind; i < argc; i++)
	{
		if (ping_host_add (ping, argv[i]) < 0)
		{
			const char *errmsg = ping_get_error (ping);

			fprintf (stderr, "Adding host `%s' failed: %s\n", argv[i], errmsg);
			continue;
		}
		else
		{
			host_num++;
		}
	}

	/* Permanently drop root privileges if we're setuid-root. */
	status = setuid (getuid ());
	if (status != 0)
	{
		fprintf (stderr, "Dropping privileges failed: %s\n",
				strerror (errno));
		exit (EXIT_FAILURE);
	}

#if _POSIX_SAVED_IDS
	saved_set_uid = (uid_t) -1;
#endif

	ping_initialize_contexts (ping);

	if (i == 0)
		return (1);

	memset (&sigint_action, '\0', sizeof (sigint_action));
	sigint_action.sa_handler = sigint_handler;
	if (sigaction (SIGINT, &sigint_action, NULL) < 0)
	{
		perror ("sigaction");
		return (1);
	}

	pre_loop_hook (ping);

	while (opt_count != 0)
	{
		int index;
		int status;

		if (gettimeofday (&tv_begin, NULL) < 0)
		{
			perror ("gettimeofday");
			return (1);
		}

		status = ping_send (ping);
		if (status == -EINTR)
		{
			continue;
		}
		else if (status < 0)
		{
			fprintf (stderr, "ping_send failed: %s\n",
					ping_get_error (ping));
			return (1);
		}

		index = 0;
		for (iter = ping_iterator_get (ping);
				iter != NULL;
				iter = ping_iterator_next (iter))
		{
			update_host_hook (iter, index);
			index++;
		}

		pre_sleep_hook (ping);

		/* Don't sleep in the last iteration */
		if (opt_count == 1)
			break;

		if (gettimeofday (&tv_end, NULL) < 0)
		{
			perror ("gettimeofday");
			return (1);
		}

		time_calc (&ts_wait, &ts_int, &tv_begin, &tv_end);

		/* printf ("Sleeping for %i.%09li seconds\n", (int) ts_wait.tv_sec, ts_wait.tv_nsec); */
		while ((status = nanosleep (&ts_wait, &ts_wait)) != 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			else
			{
				perror ("nanosleep");
				break;
			}
		}

		post_sleep_hook (ping);

		if (opt_count > 0)
			opt_count--;
	} /* while (opt_count != 0) */

	/* Returns the number of failed hosts according to -Z. */
	status = post_loop_hook (ping);

	ping_destroy (ping);

	if (status == 0)
		exit (EXIT_SUCCESS);
	else
	{
		if (status > 255)
			status = 255;
		exit (status);
	}
} /* }}} int main */

/* vim: set fdm=marker : */
