/*
 * netcat.c -- main project file
 * Part of the netcat project
 *
 * Author: Johnny Mnemonic <johnny@themnemonic.org>
 * Copyright (c) 2002 by Johnny Mnemonic
 *
 * $Id: netcat.c,v 1.20 2002-05-05 08:44:19 themnemonic Exp $
 */

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "netcat.h"
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <sys/time.h>
#include <signal.h>
#include <getopt.h>

int gatesidx = 0;		/* LSRR hop count */
int gatesptr = 4;		/* initial LSRR pointer, settable */
USHORT Single = 1;		/* zero if scanning */
unsigned int insaved = 0;	/* stdin-buffer size for multi-mode */
unsigned long bytes_sent = 0;	/* total bytes received (statistics) */
unsigned long bytes_recv = 0;	/* total bytes sent (statistics) */

/* will malloc up the following globals: */
netcat_host **gates = NULL;		/* LSRR hop hostpoop */
char *optbuf = NULL;		/* LSRR or sockopts */

/* global options flags */
bool opt_listen = FALSE;		/* listen mode */
bool opt_numeric = FALSE;	/* don't resolve hostnames */
bool opt_random = FALSE;		/* use random ports */
bool opt_udpmode = FALSE;	/* use udp protocol instead of tcp */
bool opt_telnet = FALSE;		/* answer in telnet mode */
bool opt_hexdump = FALSE;	/* hexdump traffic */
bool opt_zero = FALSE;		/* zero I/O mode (don't expect anything) */
int opt_interval = 0;		/* delay (in seconds) between lines/ports */
int opt_verbose = 0;		/* be verbose (> 1 to be MORE verbose) */
int opt_wait = 0;		/* wait time (FIXME) */
char *opt_outputfile = NULL;	/* hexdump output file */

static FILE *output_fd = NULL;	/* output fd (FIXME: i don't like this) */


/* fiddle all the file descriptors around, and hand off to another prog.  Sort
   of like a one-off "poor man's inetd". */
#if 0
void doexec(int fd)
{
  register char *p;

  dup2(fd, 0);			/* the precise order of fiddlage */
  close(fd);			/* is apparently crucial; this is */
  dup2(0, 1);			/* swiped directly out of "inetd". */
  dup2(0, 2);
  p = strrchr(pr00gie, '/');	/* shorter argv[0] */
  if (p)
    p++;
  else
    p = pr00gie;
  debug_d("gonna exec %s as %s...\n", pr00gie, p);
  execl(pr00gie, p, NULL);
  bail("exec %s failed", pr00gie);	/* this gets sent out.  Hmm... */
}				/* doexec */
#endif

/* signal handling */

static void got_term(int z)
{
  fprintf(stderr, "Terminated\n");
  exit(EXIT_FAILURE);
}

static void got_int(int z)
{
  dprintf(1, (_("Exiting.\nTotal received bytes: %ld\nTotal sent bytes: %ld\n"),
	  bytes_recv, bytes_sent));
  exit(EXIT_FAILURE);
}

/* handle stdin/stdout/network I/O. */

int readwrite(int sock)
{
  int read_ret, write_ret, pbuf_len;
  char buf[1024], *pbuf = NULL, *ptmp = NULL;
  fd_set ins;
  bool inloop = TRUE;
  struct timeval delayer;

  delayer.tv_sec = 0;
  delayer.tv_usec = 0;

  debug_v("readwrite(sock=%d)", sock);

  while (inloop) {
    FD_ZERO(&ins);
    FD_SET(sock, &ins);

    if (ptmp == NULL)
      FD_SET(0, &ins);
    else if ((delayer.tv_sec == 0) && (delayer.tv_usec == 0))
      delayer.tv_sec = opt_interval;

    debug_v("entering select()...");
    select(sock + 1, &ins, NULL, NULL,
	   (delayer.tv_sec || delayer.tv_usec ? &delayer : NULL));

    /* reading from stdin.  We support the buffered output by lines, which is
       controlled by the global variable opt_interval.  If it is set, we fetch
       the buffer in the usual way, but we park it in a temporary buffer.  Once
       finished, the buffer is flushed and everything returns normal. */
    if (FD_ISSET(0, &ins)) {
      read_ret = read(0, buf, sizeof(buf));
      debug_dv("read(stdin) = %d", read_ret);

      if (read_ret < 0) {
	perror("read(stdin)");
	exit(EXIT_FAILURE);
      }
      else if (read_ret == 0) {
	debug_v("EOF Received from stdin! (not exiting)");
	/* FIXME: So, it seems that nc110 stops from setting 0 in the &ins
	   after it got an eof.. in fact in some circumstances after the initial
	   eof it won't be recovered and will keep triggering select() for nothing. */
	/* inloop = FALSE; */
      }
      else {
	if (opt_interval) {
	  int i = 0;

	  while (i < read_ret)
	    if (buf[i++] == '\n')
	      break;

	  if (i < read_ret) {
	    pbuf_len = read_ret - i;
	    pbuf = ptmp = malloc(pbuf_len);
	    memcpy(pbuf, &buf[i], pbuf_len);
	    /* prepare the timeout timer so we don't fall back to the following
	       buffer-handling section.  We already sent out something and we
	       have to wait the right time before sending more. */
	    delayer.tv_sec = opt_interval;
	  }

	  read_ret = i;
        }
	write_ret = write(sock, buf, read_ret);
	bytes_sent += write_ret;		/* update statistics */
	debug_dv("write(net) = %d", write_ret);

	if (write_ret < 0) {
	  perror("write(net)");
	  exit(EXIT_FAILURE);
	}

	/* if the option is set, hexdump the received data */
	if (opt_hexdump) {
#ifndef USE_OLD_HEXDUMP
	  fprintf(output_fd, "Sent %u bytes to the socket\n", write_ret);
#endif
	  netcat_fhexdump(output_fd, '>', buf, write_ret);
	}

      }
    }

    /* reading from the socket (net). */
    if (FD_ISSET(sock, &ins)) {
      read_ret = read(sock, buf, sizeof(buf));
      debug_dv("read(net) = %d", read_ret);

      if (read_ret < 0) {
	perror("read(net)");
	exit(EXIT_FAILURE);
      }
      else if (read_ret == 0) {
	debug_v("EOF Received from the net");
	inloop = FALSE;
      }
      else {
	write_ret = write(1, buf, read_ret);
	bytes_recv += write_ret;
	debug_dv("write(stdout) = %d", write_ret);

	if (write_ret < 0) {
	  perror("write(stdout)");
	  exit(EXIT_FAILURE);
	}

	/* FIXME: handle write_ret != read_ret */

	/* If option is set, hexdump the received data */
	if (opt_hexdump) {
#ifndef USE_OLD_HEXDUMP
	  fprintf(output_fd, "Received %u bytes from the socket\n", write_ret);
#endif
	  netcat_fhexdump(output_fd, '<', buf, write_ret);
	}

      }
    }

    /* now handle the buffered data (if any) */
    if (ptmp && (delayer.tv_sec == 0) && (delayer.tv_usec == 0)) {
      int i = 0;

      while (i < pbuf_len)
	if (pbuf[i++] == '\n')
	  break;

      /* if this reaches 0, the buffer is over and we can clean it */
      pbuf_len -= i;

      write_ret = write(sock, pbuf, i);
      bytes_sent += write_ret;
      debug_dv("write(stdout) = %d", write_ret);

      if (write_ret < 0) {
	perror("write(stdout)");
	exit(EXIT_FAILURE);
      }

      bytes_sent += write_ret;

      if (pbuf_len == 0) {
	free(ptmp);
	ptmp = NULL;
	pbuf = NULL;
      }
      else {
	pbuf += i;
      }
    }
  }				/* end of while (inloop) */

  return 0;
}				/* end of readwrite() */

/* main: handle command line arguments and listening status */

int main(int argc, char *argv[])
{
  int c, my_fd;
  netcat_host local_host, remote_host;
  netcat_port local_port, remote_port;
  struct in_addr *ouraddr;
  struct sigaction sv;

  memset(&local_host, 0, sizeof(local_host));
  memset(&remote_host, 0, sizeof(remote_host));

#ifdef ENABLE_NLS
  setlocale(LC_MESSAGES, "");
  bindtextdomain(PACKAGE, LOCALEDIR);
  textdomain(PACKAGE);
#endif

  /* FIXME: what do i need this for? */
  res_init();

  /* set up the signal handling system */
  sigemptyset(&sv.sa_mask);
  sv.sa_flags = 0;
  sv.sa_handler = got_int;
  sigaction(SIGINT, &sv, NULL);
  sv.sa_handler = got_term;
  sigaction(SIGTERM, &sv, NULL);

  /* ignore some boring signals */
  sv.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sv, NULL);
  sigaction(SIGURG, &sv, NULL);

  /* if no args given at all, get them from stdin */
  if (argc == 1)
    netcat_commandline_read(&argc, &argv);

  /* check for command line switches */
  while (TRUE) {
    int option_index = 0;
    static const struct option long_options[] = {
	{ "exec",	required_argument,	NULL, 'e' },
	{ "gateway",	required_argument,	NULL, 'g' },
	{ "pointer",	required_argument,	NULL, 'G' },
	{ "help",	no_argument,		NULL, 'h' },
	{ "interval",	required_argument,	NULL, 'i' },
	{ "listen",	no_argument,		NULL, 'l' },
	{ "dont-resolve", no_argument,		NULL, 'n' },
	{ "output",	required_argument,	NULL, 'o' },
	{ "local-port",	required_argument,	NULL, 'p' },
	{ "randomize",	no_argument,		NULL, 'r' },
	{ "source",	required_argument,	NULL, 's' },
	{ "telnet",	no_argument,		NULL, 't' },
	{ "udp",		no_argument,		NULL, 'u' },
	{ "verbose",	no_argument,		NULL, 'v' },
	{ "version",	no_argument,		NULL, 'V' },
	{ "hexdump",	no_argument,		NULL, 'x' },
	{ "wait",	required_argument,	NULL, 'w' },
	{ "zero",	no_argument,		NULL, 'z' },
	{ 0, 0, 0, 0 }
    };

    c = getopt_long(argc, argv, "e:g:G:hi:lno:p:rs:tuvxw:z", long_options,
		    &option_index);
    if (c == -1)
      break;

    switch (c) {
    case 'e':			/* prog to exec */
      break;
    case 'G':			/* srcrt gateways pointer val */
      break;
    case 'g':			/* srcroute hop[s] */
      break;
    case 'h':			/* display help and exit */
      netcat_printhelp(argv[0]);
      exit(EXIT_SUCCESS);
    case 'i':			/* line/ports interval time (seconds) */
      opt_interval = atoi(optarg);
      if (opt_interval <= 0) {
	fprintf(stderr, _("Error: Invalid interval time \"%s\"\n"), optarg);
	exit(EXIT_FAILURE);
      }
      break;
    case 'l':			/* listen mode */
      opt_listen = TRUE;
      break;
    case 'n':			/* numeric-only, no DNS lookups */
      opt_numeric++;
      break;
    case 'o':			/* output hexdump log to file */
      opt_outputfile = strdup(optarg);
      opt_hexdump = TRUE;	/* implied */
      break;
    case 'p':			/* local source port */
      if (!netcat_getport(&local_port, optarg, 0)) {
	fprintf(stderr, _("Error: invalid local port: %s\n"), optarg);
	exit(EXIT_FAILURE);
      }
      break;
    case 'r':			/* randomize various things */
      opt_random = TRUE;
      break;
    case 's':			/* local source address */
      /* lookup the source address and assign it to the connection address */
      if (!netcat_resolvehost(&local_host, optarg)) {
	fprintf(stderr, _("Error: Couldn't resolve local host: %s\n"), optarg);
	exit(EXIT_FAILURE);
      }
      ouraddr = &local_host.iaddrs[0];
      break;
    case 't':			/* do telnet fakeout */
      opt_telnet++;
      break;
    case 'u':			/* use UDP protocol */
      opt_udpmode = TRUE;
      break;
    case 'v':			/* be verbose (twice=more verbose) */
      opt_verbose++;
      break;
    case 'V':			/* display version and exit */
      netcat_printversion();
      exit(EXIT_SUCCESS);
    case 'w':			/* wait time (in seconds) */
      opt_wait = atoi(optarg);
      if (opt_wait <= 0) {
	fprintf(stderr, _("Error: invalid wait-time: %s\n"), optarg);
	exit(EXIT_FAILURE);
      }
      break;
    case 'x':			/* hexdump traffic */
      opt_hexdump = TRUE;
      break;
    case 'z':			/* little or no data xfer */
      opt_zero++;
      break;
    default:
      fprintf(stderr, _("Try `%s --help' for more information.\n"), argv[0]);
      exit(EXIT_FAILURE);
    }
  }

  /* initialize the flag buffer to keep track of the specified ports */
  netcat_flag_init();

  /* randomize only if needed */
  if (opt_random)
    SRAND(time(0));

  if (opt_outputfile) {
    output_fd = fopen(opt_outputfile, "w");
    if (!output_fd) {
      perror(_("Failed to open output file: "));
      exit(EXIT_FAILURE);
    }
  }
  else
    output_fd = stdout;

  debug_v("Trying to parse non-args parameters (argc=%d, optind=%d)", argc, optind);

  /* try to get an hostname parameter */
  if (optind < argc) {
    char *myhost = argv[optind++];
    if (!netcat_resolvehost(&remote_host, myhost)) {
      fprintf(stderr, _("Error: Couldn't resolve host \"%s\"\n"), myhost);
      exit(EXIT_FAILURE);
    }
  }

  /* now loop all the other (maybe optional) parameters for port-ranges */
  while (optind < argc) {
    char *get_argv = argv[optind++], *q;
    char *parse = strdup(get_argv);
    int port_lo = 0, port_hi = 65535;

    if (!(q = strchr(parse, '-'))) {		/* simple number */
      if (netcat_getport(&remote_port, parse, 0))
	netcat_flag_set(remote_port.num, TRUE);
      else
	goto got_err;
    }
    else {		/* could be in the forms: N1-N2, -N2, N1- */
      *q++ = 0;
      if (*parse) {
	if (netcat_getport(&remote_port, parse, 0))
	  port_lo = remote_port.num;
	else
	  goto got_err;
      }
      if (*q) {
	if (netcat_getport(&remote_port, q, 0))
	  port_hi = remote_port.num;
	else
	  goto got_err;
      }
      /* now update the flagset (this is int, so it's ok even if hi == 65535) */
      while (port_lo <= port_hi)
	netcat_flag_set(port_lo++, TRUE);
    }

    free(parse);
    continue;

 got_err:
    fprintf(stderr, _("Error: Invalid port specification: %s\n"), get_argv);
    free(parse);
    exit(EXIT_FAILURE);
  }

  debug_dv("Arguments parsing complete! Total ports=%d", netcat_flag_count());

#if 1
  c = 0;
  while ((c = netcat_flag_next(c))) {
    printf("Got port=%d\n", c);
  }
#endif

  /* Handle listen mode */
  if (opt_listen) {
    int sock_listen;

    sock_listen = netcat_socket_new_listen(&local_host.iaddrs[0], local_port.num);

    if (sock_listen < 0) {
      fprintf(stderr, _("Error: Couldn't setup listen socket (err=%d)\n"),
	      sock_listen);
      exit(EXIT_FAILURE);
    }

    debug_dv("Entering SELECT loop");

    do {
      my_fd = netcat_socket_accept(sock_listen, opt_wait);

      if (my_fd < 0) {
        dprintf(2, (_("Listen mode failed: %s\n"), strerror(errno)));
        exit(EXIT_FAILURE);
      }
      else {
	struct sockaddr_in my_addr;
	socklen_t my_len = sizeof(my_addr);

	getpeername(my_fd, (struct sockaddr *)&my_addr, &my_len);

	/* if a remote address have been specified test it */
	if (remote_host.iaddrs[0].s_addr) {
	  /* FIXME: ALL addresses should be tried */
	  if (memcmp(&remote_host.iaddrs[0], &my_addr.sin_addr,
		     sizeof(local_host.iaddrs[0]))) {
	    dprintf(2, (_("Unwanted connection from %s:%d\n"),
		    netcat_inet_ntop(&my_addr.sin_addr), my_addr.sin_port));
	    shutdown(my_fd, 2);
	    close(my_fd);
	    continue;
	  }
	}
	dprintf(1, ("Connection from %s:%d\n", netcat_inet_ntop(&my_addr.sin_addr),
		my_addr.sin_port));
      }

      /* we don't need a listening socket anymore */
      close(sock_listen);	/* FIXME: do we have to shutdown() here? */
      break;
    } while (TRUE);

    /* entering the core loop */
    readwrite(my_fd);

    debug_dv("Listen: EXIT");
    exit(0);
  }				/* end of listen mode handling */

  /* we need to connect outside */

  /* since ports are the second argument, checking ports might be enough */
  if (netcat_flag_count() == 0) {
    fprintf(stderr, _("Error: No ports specified for connection\n"));
    exit(EXIT_FAILURE);
  }

  c = 0;
  while ((c = netcat_flag_next(c))) {
    int ret;
    fd_set ins;
    fd_set outs;

    FD_ZERO(&ins);
    FD_ZERO(&outs);

    debug_dv("Trying connection to %s[:%d]",
	     netcat_inet_ntop(&remote_host.iaddrs[0]), c);

    /* since we are nonblocking now, we can start as many connections as we want
       but it's not a great idea connecting more than one host at time */
    my_fd = netcat_socket_new_connect(&remote_host.iaddrs[0], c, NULL, 0);
    assert(my_fd > 0);

    FD_SET(my_fd, &ins);
    FD_SET(my_fd, &outs);

    /* FIXME: what happens if: Connection Refused, or calling select on an already
       connected host */
    debug_dv("Entering SELECT sock=%d", my_fd);
    select(my_fd + 1, &ins, &outs, NULL, NULL);

    /* FIXME: why do i get both these? */
    if (FD_ISSET(my_fd, &ins)) {
      char tmp;
      ret = read(my_fd, &tmp, 1);
      dprintf(2, ("%s [%s] %s : %s\n", "xx", "xx", "xx", strerror(errno)));
      ret = shutdown(my_fd, 2);
      debug_v("shutdown() = %d", ret);
      ret = close(my_fd);
      debug_v("close() = %d", ret);
      continue;
    }

    if (FD_ISSET(my_fd, &outs)) {
      debug_v("IS SET outs");

      readwrite(my_fd);

    }

    debug_dv("FINISH");

  }

  exit(0);

}				/* main */
