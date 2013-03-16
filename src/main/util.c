/*
 * util.c	Various utility functions.
 *
 * Version:     $Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 */

#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/rad_assert.h>

#include <ctype.h>
#include <signal.h>

#include <sys/stat.h>
#include <fcntl.h>

/*
 *	The signal() function in Solaris 2.5.1 sets SA_NODEFER in
 *	sa_flags, which causes grief if signal() is called in the
 *	handler before the cause of the signal has been cleared.
 *	(Infinite recursion).
 *
 *	The same problem appears on HPUX, so we avoid it, if we can.
 *
 *	Using sigaction() to reset the signal handler fixes the problem,
 *	so where available, we prefer that solution.
 */

void (*reset_signal(int signo, void (*func)(int)))(int)
{
#ifdef HAVE_SIGACTION
	struct sigaction act, oact;

	memset(&act, 0, sizeof(act));
	act.sa_handler = func;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
#ifdef  SA_INTERRUPT		/* SunOS */
	act.sa_flags |= SA_INTERRUPT;
#endif
	if (sigaction(signo, &act, &oact) < 0)
		return SIG_ERR;
	return oact.sa_handler;
#else

	/*
	 *	re-set by calling the 'signal' function, which
	 *	may cause infinite recursion and core dumps due to
	 *	stack growth.
	 *
	 *	However, the system is too dumb to implement sigaction(),
	 *	so we don't have a choice.
	 */
	signal(signo, func);

	return NULL;
#endif
}

/*
 *	Per-request data, added by modules...
 */
struct request_data_t {
	request_data_t	*next;

	void		*unique_ptr;
	int		unique_int;
	void		*opaque;
	void		(*free_opaque)(void *);
};

/*
 *	Add opaque data (with a "free" function) to a REQUEST.
 *
 *	The unique ptr is meant to be a malloc'd module configuration,
 *	and the unique integer allows the caller to have multiple
 *	opaque data associated with a REQUEST.
 */
int request_data_add(REQUEST *request,
		     void *unique_ptr, int unique_int,
		     void *opaque, void (*free_opaque)(void *))
{
	request_data_t *this, **last, *next;

	/*
	 *	Some simple sanity checks.
	 */
	if (!request || !opaque) return -1;

	this = next = NULL;
	for (last = &(request->data); *last != NULL; last = &((*last)->next)) {
		if (((*last)->unique_ptr == unique_ptr) &&
		    ((*last)->unique_int == unique_int)) {
			this = *last;

			next = this->next;

			if (this->opaque && /* free it, if necessary */
			    this->free_opaque)
				this->free_opaque(this->opaque);
			break;	/* replace the existing entry */
		}
	}

	if (!this) this = rad_malloc(sizeof(*this));
	memset(this, 0, sizeof(*this));

	this->next = next;
	this->unique_ptr = unique_ptr;
	this->unique_int = unique_int;
	this->opaque = opaque;
	this->free_opaque = free_opaque;

	*last = this;

	return 0;
}


/*
 *	Get opaque data from a request.
 */
void *request_data_get(REQUEST *request,
		       void *unique_ptr, int unique_int)
{
	request_data_t **last;

	if (!request) return NULL;

	for (last = &(request->data); *last != NULL; last = &((*last)->next)) {
		if (((*last)->unique_ptr == unique_ptr) &&
		    ((*last)->unique_int == unique_int)) {
			request_data_t *this = *last;
			void *ptr = this->opaque;

			/*
			 *	Remove the entry from the list, and free it.
			 */
			*last = this->next;
			free(this);
			return ptr; /* don't free it, the caller does that */
		}
	}

	return NULL;		/* wasn't found, too bad... */
}


/*
 *	Get opaque data from a request without removing it.
 */
void *request_data_reference(REQUEST *request,
		       void *unique_ptr, int unique_int)
{
	request_data_t **last;

	for (last = &(request->data); *last != NULL; last = &((*last)->next)) {
		if (((*last)->unique_ptr == unique_ptr) &&
		    ((*last)->unique_int == unique_int)) {
			request_data_t *this = *last;
			void *ptr = this->opaque;

			return ptr;
		}
	}

	return NULL;		/* wasn't found, too bad... */
}


/*
 *	Free a REQUEST struct.
 */
void request_free(REQUEST **request_ptr)
{
	REQUEST *request;

	if (!request_ptr || !*request_ptr) {
		return;
	}

	request = *request_ptr;

	rad_assert(!request->in_request_hash);
#ifdef WITH_PROXY
	rad_assert(!request->in_proxy_hash);
#endif
	rad_assert(!request->ev);

	if (request->packet)
		rad_free(&request->packet);

#ifdef WITH_PROXY
	if (request->proxy)
		rad_free(&request->proxy);
#endif

	if (request->reply)
		rad_free(&request->reply);

#ifdef WITH_PROXY
	if (request->proxy_reply)
		rad_free(&request->proxy_reply);
#endif

	if (request->config_items)
		pairfree(&request->config_items);

	request->username = NULL;
	request->password = NULL;

	if (request->data) {
		request_data_t *this, *next;

		for (this = request->data; this != NULL; this = next) {
			next = this->next;
			if (this->opaque && /* free it, if necessary */
			    this->free_opaque)
				this->free_opaque(this->opaque);
			free(this);
		}
		request->data = NULL;
	}

	if (request->root &&
	    (request->root->refcount > 0)) {
		request->root->refcount--;
		request->root = NULL;
	}

#ifdef WITH_COA
	if (request->coa) {
		request->coa->parent = NULL;
		rad_assert(request->coa->ev == NULL);
		request_free(&request->coa);
	}

	if (request->parent && (request->parent->coa == request)) {
		request->parent->coa = NULL;
	}
#endif

#ifndef NDEBUG
	request->magic = 0x01020304;	/* set the request to be nonsense */
#endif
	request->client = NULL;
#ifdef WITH_PROXY
	request->home_server = NULL;
#endif
	talloc_free(request);
	*request_ptr = NULL;
}

/*
 *	Check a filename for sanity.
 *
 *	Allow only uppercase/lowercase letters, numbers, and '-_/.'
 */
int rad_checkfilename(const char *filename)
{
	if (strspn(filename, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_/.") == strlen(filename)) {
		return 0;
	}

	return -1;
}

/** Check if file exists
 *
 * @param filename to check.
 * @return 0 if the file does not exist, 1 if the file exists, -1 if the file
 *	exists but there was an error opening it. errno value should be usable
 *	for error messages.
 */
int rad_file_exists(const char *filename)
{
	int des;
	int ret = 1;
	
	if ((des = open(filename, O_RDONLY)) == -1) {
		if (errno == ENOENT) {
			ret = 0;
		} else {
			ret = -1;
		}
	} else {
		close(des);
	}
	
	return ret;
}

/*
 *	Create possibly many directories.
 *
 *	Note that the input directory name is NOT a constant!
 *	This is so that IF an error is returned, the 'directory' ptr
 *	points to the name of the file which caused the error.
 */
int rad_mkdir(char *directory, mode_t mode)
{
	int rcode;
	char *p;
	struct stat st;

	/*
	 *	If the directory exists, don't do anything.
	 */
	if (stat(directory, &st) == 0) {
		return 0;
	}

	/*
	 *	Look for the LAST directory name.  Try to create that,
	 *	failing on any error.
	 */
	p = strrchr(directory, FR_DIR_SEP);
	if (p != NULL) {
		*p = '\0';
		rcode = rad_mkdir(directory, mode);

		/*
		 *	On error, we leave the directory name as the
		 *	one which caused the error.
		 */
		if (rcode < 0) {
			if (errno == EEXIST) return 0;
			return rcode;
		}

		/*
		 *	Reset the directory delimiter, and go ask
		 *	the system to make the directory.
		 */
		*p = FR_DIR_SEP;
	} else {
		return 0;
	}

	/*
	 *	Having done everything successfully, we do the
	 *	system call to actually go create the directory.
	 */
	rcode = mkdir(directory, mode & 0777);
	if (rcode < 0) {
		return rcode;
	}
	
	/*
	 *	Set things like sticky bits that aren't supported by
	 *	mkdir.
	 */
	if (mode & ~0777) {
		rcode = chmod(directory, mode);
	}
	
	return rcode;
}


/*
 *	Module malloc() call, which does stuff if the malloc fails.
 *
 *	This call ALWAYS succeeds!
 */
void *rad_malloc(size_t size)
{
	void *ptr = malloc(size);

	if (ptr == NULL) {
		radlog(L_ERR, "no memory");
		exit(1);
	}

	return ptr;
}


void *rad_calloc(size_t size)
{
	void *ptr = rad_malloc(size);
	memset(ptr, 0, size);
	return ptr;
}

void rad_const_free(const void *ptr)
{
	void *tmp;
	if (!ptr) return;

	memcpy(&tmp, &ptr, sizeof(tmp));
	talloc_free(tmp);
}


/*
 *	Signature for free is dumb, and raises errors when we try
 *	to free const ptrs.
 */
void rad_cfree(const void *ptr)
{
	void *tmp;
	if (!ptr) return;

	memcpy(&tmp, &ptr, sizeof(tmp));
	free(tmp);
}

/*
 *	Logs an error message and aborts the program
 *
 */

void NEVER_RETURNS rad_assert_fail (const char *file, unsigned int line,
				    const char *expr)
{
	radlog(L_ERR, "ASSERT FAILED %s[%u]: %s", file, line, expr);
	abort();
}


/*
 *	Create a new REQUEST data structure.
 */
REQUEST *request_alloc(void)
{
	REQUEST *request;

	request = talloc_zero(NULL, REQUEST);
#ifndef NDEBUG
	request->magic = REQUEST_MAGIC;
#endif
#ifdef WITH_PROXY
	request->proxy = NULL;
#endif
	request->reply = NULL;
#ifdef WITH_PROXY
	request->proxy_reply = NULL;
#endif
	request->config_items = NULL;
	request->username = NULL;
	request->password = NULL;
	request->timestamp = time(NULL);
	request->options = debug_flag; /* Default to global debug level */

	request->module = "";
	request->component = "<core>";
	if (debug_flag) request->radlog = radlog_request;

	return request;
}


/*
 *	Create a new REQUEST, based on an old one.
 *
 *	This function allows modules to inject fake requests
 *	into the server, for tunneled protocols like TTLS & PEAP.
 */
REQUEST *request_alloc_fake(REQUEST *request)
{
  REQUEST *fake;

  fake = request_alloc();

  fake->number = request->number;
#ifdef HAVE_PTHREAD_H
  fake->child_pid = request->child_pid;
#endif
  fake->parent = request;
  fake->root = request->root;
  fake->client = request->client;

  /*
   *	For new server support.
   *
   *	FIXME: Key instead off of a "virtual server" data structure.
   *
   *	FIXME: Permit different servers for inner && outer sessions?
   */
  fake->server = request->server;

  fake->packet = rad_alloc(request, 1);
  if (!fake->packet) {
	  request_free(&fake);
	  return NULL;
  }

  fake->reply = rad_alloc(request, 0);
  if (!fake->reply) {
	  request_free(&fake);
	  return NULL;
  }

  fake->master_state = REQUEST_ACTIVE;
  fake->child_state = REQUEST_RUNNING;

  /*
   *	Fill in the fake request.
   */
  fake->packet->sockfd = -1;
  fake->packet->src_ipaddr = request->packet->src_ipaddr;
  fake->packet->src_port = request->packet->src_port;
  fake->packet->dst_ipaddr = request->packet->dst_ipaddr;
  fake->packet->dst_port = 0;

  /*
   *	This isn't STRICTLY required, as the fake request MUST NEVER
   *	be put into the request list.  However, it's still reasonable
   *	practice.
   */
  fake->packet->id = fake->number & 0xff;
  fake->packet->code = request->packet->code;
  fake->timestamp = request->timestamp;

  /*
   *	Required for new identity support
   */
  fake->listener = request->listener;

  /*
   *	Fill in the fake reply, based on the fake request.
   */
  fake->reply->sockfd = fake->packet->sockfd;
  fake->reply->src_ipaddr = fake->packet->dst_ipaddr;
  fake->reply->src_port = fake->packet->dst_port;
  fake->reply->dst_ipaddr = fake->packet->src_ipaddr;
  fake->reply->dst_port = fake->packet->src_port;
  fake->reply->id = fake->packet->id;
  fake->reply->code = 0; /* UNKNOWN code */

  /*
   *	Copy debug information.
   */
  fake->options = request->options;
  fake->radlog = request->radlog;

  return fake;
}

#ifdef WITH_COA
REQUEST *request_alloc_coa(REQUEST *request)
{
	if (!request || request->coa) return NULL;

	/*
	 *	Originate CoA requests only when necessary.
	 */
	if ((request->packet->code != PW_AUTHENTICATION_REQUEST) &&
	    (request->packet->code != PW_ACCOUNTING_REQUEST)) return NULL;

	request->coa = request_alloc_fake(request);
	if (!request->coa) return NULL;

	request->coa->packet->code = 0; /* unknown, as of yet */
	request->coa->child_state = REQUEST_RUNNING;
	request->coa->proxy = rad_alloc(request->coa, 0);
	if (!request->coa->proxy) {
		request_free(&request->coa);
		return NULL;
	}

	return request->coa;
}
#endif

/*
 *	Copy a quoted string.
 */
int rad_copy_string(char *to, const char *from)
{
	int length = 0;
	char quote = *from;

	do {
		if (*from == '\\') {
			*(to++) = *(from++);
			length++;
		}
		*(to++) = *(from++);
		length++;
	} while (*from && (*from != quote));

	if (*from != quote) return -1; /* not properly quoted */

	*(to++) = quote;
	length++;
	*to = '\0';

	return length;
}

/*
 *	Copy a quoted string but without the quotes. The length
 *	returned is the number of chars written; the number of
 *	characters consumed is 2 more than this.
 */
int rad_copy_string_bare(char *to, const char *from)
{
	int length = 0;
	char quote = *from;

	from++;
	while (*from && (*from != quote)) {
		if (*from == '\\') {
			*(to++) = *(from++);
			length++;
		}
		*(to++) = *(from++);
		length++;
	}

	if (*from != quote) return -1; /* not properly quoted */

	*to = '\0';

	return length;
}


/*
 *	Copy a %{} string.
 */
int rad_copy_variable(char *to, const char *from)
{
	int length = 0;
	int sublen;

	*(to++) = *(from++);
	length++;

	while (*from) {
		switch (*from) {
		case '"':
		case '\'':
			sublen = rad_copy_string(to, from);
			if (sublen < 0) return sublen;
			from += sublen;
			to += sublen;
			length += sublen;
			break;

		case '}':	/* end of variable expansion */
			*(to++) = *(from++);
			*to = '\0';
			length++;
			return length; /* proper end of variable */

		case '\\':
			*(to++) = *(from++);
			*(to++) = *(from++);
			length += 2;
			break;

		case '%':	/* start of variable expansion */
			if (from[1] == '{') {
				*(to++) = *(from++);
				length++;

				sublen = rad_copy_variable(to, from);
				if (sublen < 0) return sublen;
				from += sublen;
				to += sublen;
				length += sublen;
				break;
			} /* else FIXME: catch %%{ ?*/

			/* FALL-THROUGH */
		default:
			*(to++) = *(from++);
			length++;
			break;
		}
	} /* loop over the input string */

	/*
	 *	We ended the string before a trailing '}'
	 */

	return -1;
}

#ifndef USEC
#define USEC 1000000
#endif

int rad_pps(int *past, int *present, time_t *then, struct timeval *now)
{
	int pps;

	if (*then != now->tv_sec) {
		*then = now->tv_sec;
		*past = *present;
		*present = 0;
	}

	/*
	 *	Bootstrap PPS by looking at a percentage of
	 *	the previous PPS.  This lets us take a moving
	 *	count, without doing a moving average.  If
	 *	we're a fraction "f" (0..1) into the current
	 *	second, we can get a good guess for PPS by
	 *	doing:
	 *
	 *	PPS = pps_now + pps_old * (1 - f)
	 *
	 *	It's an instantaneous measurement, rather than
	 *	a moving average.  This will hopefully let it
	 *	respond better to sudden spikes.
	 *
	 *	Doing the calculations by thousands allows us
	 *	to not overflow 2^32, AND to not underflow
	 *	when we divide by USEC.
	 */
	pps = USEC - now->tv_usec; /* useconds left in previous second */
	pps /= 1000;		   /* scale to milliseconds */
	pps *= *past;		   /* multiply by past count to get fraction */
	pps /= 1000;		   /* scale to usec again */
	pps += *present;	   /* add in current count */

	return pps;
}

/*
 * Split a string into words, xlat each one and write into argv array.
 * Return argc or -1 on failure.
 */

int rad_expand_xlat(REQUEST *request, const char *cmd,
		    int max_argc, const char *argv[], int can_fail,
		    size_t argv_buflen, char *argv_buf)
{
	const char *from;
	char *to;
	int argc = -1;
	int i;
	int left;

	if (strlen(cmd) > (argv_buflen - 1)) {
		radlog(L_ERR, "rad_expand_xlat: Command line is too long");
		return -1;
	}

	/*
	 *	Check for bad escapes.
	 */
	if (cmd[strlen(cmd) - 1] == '\\') {
		radlog(L_ERR, "rad_expand_xlat: Command line has final backslash, without a following character");
		return -1;
	}

	strlcpy(argv_buf, cmd, argv_buflen);

	/*
	 *	Split the string into argv's BEFORE doing radius_xlat...
	 */
	from = cmd;
	to = argv_buf;
	argc = 0;
	while (*from) {
		int length;

		/*
		 *	Skip spaces.
		 */
		if ((*from == ' ') || (*from == '\t')) {
			from++;
			continue;
		}

		argv[argc] = to;
		argc++;

		if (argc >= (max_argc - 1)) break;

		/*
		 *	Copy the argv over to our buffer.
		 */
		while (*from && (*from != ' ') && (*from != '\t')) {
			if (to >= argv_buf + argv_buflen - 1) {
				radlog(L_ERR, "rad_expand_xlat: Ran out of space in command line");
				return -1;
			}

			switch (*from) {
			case '"':
			case '\'':
				length = rad_copy_string_bare(to, from);
				if (length < 0) {
					radlog(L_ERR, "rad_expand_xlat: Invalid string passed as argument");
					return -1;
				}
				from += length+2;
				to += length;
				break;

			case '%':
				if (from[1] == '{') {
					*(to++) = *(from++);

					length = rad_copy_variable(to, from);
					if (length < 0) {
						radlog(L_ERR, "rad_expand_xlat: Invalid variable expansion passed as argument");
						return -1;
					}
					from += length;
					to += length;
				} else { /* FIXME: catch %%{ ? */
					*(to++) = *(from++);
				}
				break;

			case '\\':
				if (from[1] == ' ') from++;
				/* FALL-THROUGH */

			default:
				*(to++) = *(from++);
			}
		} /* end of string, or found a space */

		*(to++) = '\0';	/* terminate the string */
	}

	/*
	 *	We have to have SOMETHING, at least.
	 */
	if (argc <= 0) {
		radlog(L_ERR, "rad_expand_xlat: Empty command line.");
		return -1;
	}

	/*
	 *	Expand each string, as appropriate.
	 */
	left = argv_buf + argv_buflen - to;
	for (i = 0; i < argc; i++) {
		int sublen;

		/*
		 *	Don't touch argv's which won't be translated.
		 */
		if (strchr(argv[i], '%') == NULL) continue;

		if (!request) continue;

		sublen = radius_xlat(to, left - 1, argv[i], request, NULL, NULL);
		if (sublen <= 0) {
			if (can_fail) {
				/*
				 *	Fail to be backwards compatible.
				 *
				 *	It's yucky, but it won't break anything,
				 *	and it won't cause security problems.
				 */
				sublen = 0;
			} else {
				radlog(L_ERR, "rad_expand_xlat: xlat failed");
				return -1;
			}
		}

		argv[i] = to;
		to += sublen;
		*(to++) = '\0';
		left -= sublen;
		left--;

		if (left <= 0) {
			radlog(L_ERR, "rad_expand_xlat: Ran out of space while expanding arguments.");
			return -1;
		}
	}
	argv[argc] = NULL;

	return argc;
}

