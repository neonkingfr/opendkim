/*
**  Copyright (c) 2008 Sendmail, Inc. and its suppliers.
**	All rights reserved.
**
**  Copyright (c) 2009, The OpenDKIM Project.  All rights reserved.
**
**  $Id: opendkim-db.c,v 1.7 2009/10/28 15:40:07 cm-msk Exp $
*/

#ifndef lint
static char opendkim_db_c_id[] = "@(#)$Id: opendkim-db.c,v 1.7 2009/10/28 15:40:07 cm-msk Exp $";
#endif /* !lint */

/* system includes */
#include <sys/types.h>
#include <sys/file.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <regex.h>
#ifdef USE_DB
# include <db.h>
#endif /* USE_DB */

/* opendkim includes */
#include "opendkim-db.h"
#include "opendkim.h"
#include "util.h"

/* macros */
#define DKIMF_DB_MODE		0644

#define	DKIMF_DB_TYPE_UNKNOWN	(-1)
#define	DKIMF_DB_TYPE_FILE	0
#define	DKIMF_DB_TYPE_REFILE	1
#define	DKIMF_DB_TYPE_CSL	2
#ifdef USE_DB
# define DKIMF_DB_TYPE_BDB	3
#endif /* USE_DB */

#define	DKIMF_DB_IFLAG_FREEARRAY 0x01

#ifdef USE_DB
# ifndef DB_NOTFOUND
#  define DB_NOTFOUND		1
# endif /* ! DB_NOTFOUND */
# ifndef DB_VERSION_MAJOR
#  define DB_VERSION_MAJOR	1
# endif /* ! DB_VERSION_MAJOR */

# define DB_VERSION_CHECK(x,y,z) ((DB_VERSION_MAJOR == (x) && \
				   DB_VERSION_MINOR == (y) && \
				   DB_VERSION_PATCH >= (z)) || \
				  (DB_VERSION_MAJOR == (x) && \
				   DB_VERSION_MINOR > (y)) || \
				  DB_VERSION_MAJOR > (x))

# if DB_VERSION_CHECK(3,0,0)
#  define DB_STRERROR(x)	db_strerror(x)
# else /* DB_VERSION_CHECK(3,0,0) */
#  define DB_STRERROR(x)	strerror(errno)
# endif /* DB_VERSION_CHECK(3,0,0) */

# if DB_VERSION_MAJOR < 2
#  define DKIMF_DBCLOSE(db)	(db)->close((db))
# else /* DB_VERSION_MAJOR < 2 */
#  define DKIMF_DBCLOSE(db)	(db)->close((db), 0)
# endif /* DB_VERSION_MAJOR < 2 */
#endif /* USE_DB */

/* data types */
struct dkim_db
{
	u_int			db_flags;
	u_int			db_iflags;
	u_int			db_type;
	int			db_status;
	int			db_nrecs;
	pthread_mutex_t *	db_lock;
	void *			db_handle;
	void *			db_data;
	void *			db_cursor;
	char **			db_array;
};

struct dkimf_db_table
{
	char *			name;
	int 			code;
};

struct dkimf_db_list
{
	char *			db_list_key;
	char *			db_list_value;
	struct dkimf_db_list *	db_list_next;
};

struct dkimf_db_relist
{
	regex_t			db_relist_re;
	struct dkimf_db_relist * db_relist_next;
};

/* globals */
struct dkimf_db_table dbtypes[] =
{
	{ "csl",		DKIMF_DB_TYPE_CSL },
	{ "file",		DKIMF_DB_TYPE_FILE },
	{ "refile",		DKIMF_DB_TYPE_REFILE },
#ifdef DKIMF_DB_TYPE_BDB
	{ "db",			DKIMF_DB_TYPE_BDB },
#endif /* DKIMF_DB_TYPE_BDB */
};
#define DKIMF_DB_TYPE_COUNT	4

/*
**  DKIMF_DB_LIST_FREE -- destroy a linked list
**
**  Parameters:
**  	list -- list handle
**
**  Return value:
**  	None.
*/

static void
dkimf_db_list_free(struct dkimf_db_list *list)
{
	struct dkimf_db_list *next;

	assert(list != NULL);

	while (list != NULL)
	{
		free(list->db_list_key);
		if (list->db_list_value != NULL)
			free(list->db_list_value);
		next = list->db_list_next;
		free(list);
		list = next;
	}
}
		
/*
**  DKIMF_DB_RELIST_FREE -- destroy a linked regex list
**
**  Parameters:
**  	list -- list handle
**
**  Return value:
**  	None.
*/

static void
dkimf_db_relist_free(struct dkimf_db_relist *list)
{
	struct dkimf_db_relist *next;

	assert(list != NULL);

	while (list != NULL)
	{
		regfree(&list->db_relist_re);
		next = list->db_relist_next;
		free(list);
		list = next;
	}
}
		
/*
**  DKIMF_DB_OPEN -- open a database
**
**  Parameters:
**  	db -- DKIM_DB handle (returned)
**  	name -- name of DB to open
**  	flags -- operational flags
**  	lock -- lock to use during operations
**
**  Return value:
**  	3 -- other open error
**  	2 -- illegal request (e.g. writable flat file)
**  	1 -- unknown database type
**  	0 -- success
**   	-1 -- failure; check errno
**
**  Notes:
**  	The type of the database is implied by a leading "type:" string
**  	as part of "name".  The list of valid types is listed in the
**  	"dbtypes" table above.  Without such a prefix, a name that starts
**  	with "/" implies "file", otherwise "csl" is used.
**
**  	Currently defined types:
**  	csl -- "name" contains a comma-separated list
**  	file -- a flat file; may be simply a list of names if only a
**  	        memership test is needed, or it can be "key value" lines
**  	        in which case dkimf_db_get() can be used to extract the
**  	        value of a named key
**  	refile -- a flat file containing patterns (i.e. strings with the
**  	          wildcard "*"); only membership tests are allowed
**  	db -- a Sleepycat hash or b-tree database file, which can be used
**  	      for membership tests or key-value pairs
*/

int
dkimf_db_open(DKIM_DB *db, char *name, u_int flags, pthread_mutex_t *lock)
{
	DKIM_DB new;
	char *p;

	assert(db != NULL);
	assert(name != NULL);

	new = (DKIM_DB) malloc(sizeof(struct dkim_db));
	if (new == NULL)
		return -1;

	memset(new, '\0', sizeof(struct dkim_db));

	new->db_lock = lock;
	new->db_flags = flags;
	new->db_type = DKIMF_DB_TYPE_UNKNOWN;

	p = strchr(name, ':');
	if (p == NULL)
	{
# ifdef DKIMF_DB_TYPE_BDB
		char *q;

		q = NULL;
		for (p = strstr(name, ".db");
		     p != NULL;
		     p = strstr(p + 1, ".db"))
			q = p;
		if (q != NULL && *(q + 3) == '\0')
			new->db_type = DKIMF_DB_TYPE_BDB;
		else
# endif /* DKIMF_DB_TYPE_BDB */
		if (name[0] == '/')
			new->db_type = DKIMF_DB_TYPE_FILE;
		else
			new->db_type = DKIMF_DB_TYPE_CSL;
		p = name;
	}
	else
	{
		int c;
		size_t clen;
		char dbtype[BUFRSZ + 1];

		memset(dbtype, '\0', sizeof dbtype);
		clen = MIN(sizeof(dbtype) - 1, name - p);
		strncpy(dbtype, name, clen);

		for (c = 0; c < DKIMF_DB_TYPE_COUNT; c++)
		{
			if (strcasecmp(dbtypes[c].name, dbtype) == 0)
				new->db_type = dbtypes[c].code;
		}

		if (new->db_type == DKIMF_DB_TYPE_UNKNOWN)
		{
			free(new);
			return 1;
		}

		p++;
	}

	switch (new->db_type)
	{
	  case DKIMF_DB_TYPE_CSL:
	  {
		int n = 0;
		char *tmp;
		char *eq;
		char *ctx;
		struct dkimf_db_list *list = NULL;
		struct dkimf_db_list *next = NULL;
		struct dkimf_db_list *newl;

		tmp = strdup(p);
		if (tmp == NULL)
			return -1;

		for (p = strtok_r(tmp, ",", &ctx);
		     p != NULL;
		     p = strtok_r(NULL, ",", &ctx))
		{
			eq = strchr(p, '=');
			if (eq != NULL)
				*eq = '\0';

			if (eq != NULL &&
			    (new->db_flags & DKIMF_DB_FLAG_VALLIST) != 0)
			{
				char *q;
				char *ctx2;

				for (q = strtok_r(eq + 1, "|", &ctx2);
				     q != NULL;
				     q = strtok_r(NULL, "|", &ctx2))
				{
					newl = (struct dkimf_db_list *) malloc(sizeof(struct dkimf_db_list));
					if (newl == NULL)
					{
						if (list != NULL)
							dkimf_db_list_free(list);
						return -1;
					}

					newl->db_list_key = strdup(p);
					if (newl->db_list_key == NULL)
					{
						free(newl);
						if (list != NULL)
							dkimf_db_list_free(list);
						return -1;
					}

					newl->db_list_value = strdup(q);
					if (newl->db_list_value == NULL)
					{
						free(newl->db_list_key);
						free(newl);
						if (list != NULL)
							dkimf_db_list_free(list);
						return -1;
					}

					if (list == NULL)
						list = newl;
					else
						next->db_list_next = newl;

					next = newl;
					n++;
				}
			}
			else
			{
				newl = (struct dkimf_db_list *) malloc(sizeof(struct dkimf_db_list));
				if (newl == NULL)
				{
					if (list != NULL)
						dkimf_db_list_free(list);
					return -1;
				}

				newl->db_list_key = strdup(p);
				if (newl->db_list_key == NULL)
				{
					free(newl);
					if (list != NULL)
						dkimf_db_list_free(list);
					return -1;
				}

				if (eq != NULL)
				{
					newl->db_list_value = strdup(eq + 1);
					if (newl->db_list_value == NULL)
					{
						free(newl->db_list_key);
						free(newl);
						if (list != NULL)
							dkimf_db_list_free(list);
						return -1;
					}
				}

				if (list == NULL)
					list = newl;
				else
					next->db_list_next = newl;

				next = newl;
				n++;
			}
		}

		new->db_handle = list;
		new->db_nrecs = n;

		break;
	  }

	  case DKIMF_DB_TYPE_FILE:
	  {
		_Bool gapfound;
		int n = 0;
		size_t len;
		FILE *f;
		char *key;
		char *value;
		struct dkimf_db_list *list = NULL;
		struct dkimf_db_list *next = NULL;
		struct dkimf_db_list *newl;
		char line[BUFRSZ + 1];

		if ((flags & DKIMF_DB_FLAG_READONLY) == 0)
		{
			free(new);
			return 2;
		}

		f = fopen(p, "r");
		if (f == NULL)
		{
			free(new);
			return -1;
		}

		memset(line, '\0', sizeof line);
		while (fgets(line, BUFRSZ, f) != NULL)
		{
			for (p = line; *p != '\0'; p++)
			{
				if (*p == '\n' || *p == '#')
				{
					*p = '\0';
					break;
				}
			}

			dkimf_trimspaces((u_char *) line);
			if (strlen(line) == 0)
				continue;

			newl = (struct dkimf_db_list *) malloc(sizeof(struct dkimf_db_list));
			if (newl == NULL)
			{
				if (list != NULL)
					dkimf_db_list_free(list);
				fclose(f);
				return -1;
			}

			key = NULL;
			value = NULL;
			gapfound = FALSE;

			for (p = line; *p != '\0'; p++)
			{
				if (!isascii(*p) || !isspace(*p))
				{
					if (key == NULL)
						key = p;
					else if (value == NULL && gapfound)
						value = p;
				}
				else if (key != NULL && value == NULL)
				{
					*p = '\0';
					gapfound = TRUE;
				}
			}

			assert(key != NULL);

			
			if (value != NULL &&
			    (new->db_flags & DKIMF_DB_FLAG_VALLIST) != 0)
			{
				char *q;
				char *ctx;

				for (q = strtok_r(value, "|", &ctx);
				     q != NULL;
				     q = strtok_r(NULL, "|", &ctx))
				{
					newl = (struct dkimf_db_list *) malloc(sizeof(struct dkimf_db_list));
					if (newl == NULL)
					{
						if (list != NULL)
							dkimf_db_list_free(list);
						return -1;
					}

					newl->db_list_key = strdup(p);
					if (newl->db_list_key == NULL)
					{
						free(newl);
						if (list != NULL)
							dkimf_db_list_free(list);
						return -1;
					}

					newl->db_list_value = strdup(q);
					if (newl->db_list_value == NULL)
					{
						free(newl->db_list_key);
						free(newl);
						if (list != NULL)
							dkimf_db_list_free(list);
						return -1;
					}

					if (list == NULL)
						list = newl;
					else
						next->db_list_next = newl;
	
					next = newl;
					n++;
				}
			}
			else
			{
				newl->db_list_key = strdup(key);
				if (newl->db_list_key == NULL)
				{
					free(newl);
					if (list != NULL)
						dkimf_db_list_free(list);
					fclose(f);
					return -1;
				}

				if (value != NULL)
				{
					newl->db_list_value = strdup(value);
					if (newl->db_list_value == NULL)
					{
						free(newl->db_list_key);
						free(newl);
						if (list != NULL)
							dkimf_db_list_free(list);
						fclose(f);
						return -1;
					}
				}

				if (list == NULL)
					list = newl;
				else
					next->db_list_next = newl;

				next = newl;
				n++;
			}
		}

		fclose(f);

		new->db_handle = list;
		new->db_nrecs = n;

		break;
	  }

	  case DKIMF_DB_TYPE_REFILE:
	  {
		int n;
		int c;
		int status;
		int reflags;
		size_t len;
		FILE *f;
		char *end;
		struct dkimf_db_relist *list = NULL;
		struct dkimf_db_relist *newl;
		char line[BUFRSZ + 1];
		char patbuf[BUFRSZ + 1];

		if ((flags & DKIMF_DB_FLAG_READONLY) == 0)
		{
			free(new);
			return 2;
		}

		f = fopen(p, "r");
		if (f == NULL)
		{
			free(new);
			return -1;
		}

		reflags = REG_EXTENDED;
		if ((new->db_flags & DKIMF_DB_FLAG_ICASE) != 0)
			reflags |= REG_ICASE;

		memset(line, '\0', sizeof line);
		while (fgets(line, BUFRSZ, f) != NULL)
		{
			for (p = line; *p != '\0'; p++)
			{
				if (*p == '\n' || *p == '#')
				{
					*p = '\0';
					break;
				}
			}

			dkimf_trimspaces((u_char *) line);
			if (strlen(line) == 0)
				continue;

			newl = (struct dkimf_db_relist *) malloc(sizeof(struct dkimf_db_relist));
			if (newl == NULL)
			{
				if (list != NULL)
					dkimf_db_relist_free(list);
				fclose(f);
				return -1;
			}

			memset(patbuf, '\0', sizeof patbuf);
			end = patbuf + sizeof patbuf;
			patbuf[0] = '^';

			if (!dkimf_mkregexp(line, patbuf, sizeof patbuf))
			{
				if (list != NULL)
					dkimf_db_relist_free(list);
				fclose(f);
				return -1;
			}

			status = regcomp(&newl->db_relist_re, patbuf, reflags);
			if (status != 0)
			{
				new->db_data = (void *) &newl->db_relist_re;
				/* XXX -- do something */
				if (list != NULL)
					dkimf_db_relist_free(list);
				fclose(f);
				return -1;
			}

			newl->db_relist_next = list;
			list = newl;
		}

		fclose(f);

		new->db_handle = list;

		break;
	  }

#ifdef DKIMF_DB_TYPE_BDB
	  case DKIMF_DB_TYPE_BDB:
	  {
# if DB_VERSION_CHECK(2,0,0)
		int dbflags = 0;
# endif /* DB_VERSION_CHECK(2,0,0) */
		int status = 0;
		DBTYPE bdbtype;
		DB *newdb;

# if DB_VERSION_CHECK(2,0,0)
		if (flags & DKIMF_DB_FLAG_READONLY)
		{
			dbflags |= DB_RDONLY;
			bdbtype = DB_UNKNOWN;
		}
		else
		{
			dbflags |= DB_CREATE;
			bdbtype = DB_HASH;
		}
# else /* DB_VERSION_CHECK(2,0,0) */
		bdbtype = DB_HASH;
# endif /* DB_VERSION_CHECK(2,0,0) */

# if DB_VERSION_CHECK(3,0,0)
		status = db_create(&newdb, NULL, 0);
		if (status == 0)
		{
#  if DB_VERSION_CHECK(4,1,25)
 			status = newdb->open(newdb, NULL, p, NULL,
			                       bdbtype, dbflags, 0);
#  else /* DB_VERSION_CHECK(4,1,25) */
			status = newdb->open(newdb, p, NULL, bdbtype,
			                     dbflags, 0);
#  endif /* DB_VERSION_CHECK(4,1,25) */
		}
# elif DB_VERSION_CHECK(2,0,0)
		status = db_open(p, bdbtype, dbflags, DKIMF_DB_MODE,
		                 NULL, NULL, newdb);
# else /* DB_VERSION_MAJOR < 2 */
		newdb = dbopen(p,
		               (flags & DKIMF_DB_FLAGS_READONLY ? O_RDONLY
		                                                : (O_CREAT|O_RDWR)),
		               DKIMF_DB_MODE, bdbtype, NULL);
		if (newdb == NULL)
			status = errno;
# endif /* DB_VERSION_CHECK */

		if (status != 0)
			return 3;

		new->db_handle = newdb;
	  }
#endif /* DKIMF_DB_TYPE_BDB */
	}

	*db = new;
	return 0;
}

/*
**  DKIMF_DB_DELETE -- delete a key/data pair from an open database
**
**  Parameters:
**  	db -- DB handle to use for searching
**  	buf -- pointer to record to be deleted
**  	buflen -- size of record at "buf"; if 0, use strlen()
**
**  Return value:
**  	0 -- operation successful
**	!0 -- error occurred; error code returned
*/

int
dkimf_db_delete(DKIM_DB db, void *buf, size_t buflen)
{
#ifdef DKIMF_DB_TYPE_BDB
	DBT q;
	int fd;
	int status;
	int ret;
	DB *bdb;
#endif /* DKIMF_DB_TYPE_BDB */

	assert(db != NULL);
	assert(buf != NULL);

	if (db->db_type == DKIMF_DB_TYPE_FILE ||
	    db->db_type == DKIMF_DB_TYPE_CSL || 
	    db->db_type == DKIMF_DB_TYPE_REFILE)
		return EINVAL;

#ifdef DKIMF_DB_TYPE_BDB
	bdb = (DB *) db->db_handle;

	memset(&q, 0, sizeof q);
	q.data = (char *) buf;
	q.size = (buflen == 0 ? strlen(q.data) : buflen);

	ret = 0;

	/* establish write-lock */
	fd = -1;
# if DB_VERSION_CHECK(2,0,0)
	status = bdb->fd(bdb, &fd);
# else /* DB_VERSION_CHECK(2,0,0) */
	status = 0;
	fd = bdb->fd(bdb);
# endif /* DB_VERSION_CHECK(2,0,0) */

	if (db->db_lock != NULL)
		(void) pthread_mutex_lock(db->db_lock);

	if (status == 0 && fd != -1)
	{
# ifdef LOCK_EX
		status = flock(fd, LOCK_EX);
		if (status != 0)
		{
			db->db_status = status;
			return -1;
		}
# else /* LOCK_EX */
		struct flock l;

		l.l_start = 0;
		l.l_len = 0;
		l.l_type = F_WRLCK;
		l.l_whence = SEEK_SET;

		status = fcntl(fd, F_SETLKW, &l);
		if (status != 0)
		{
			db->db_status = status;
			return -1;
		}
# endif /* LOCK_EX */
	}

# if DB_VERSION_CHECK(2,0,0)
	status = bdb->del(bdb, NULL, &q, 0);
	if (status == 0)
		ret = 0;
	else
		ret = status;
# else /* DB_VERSION_CHECK(2,0,0) */
	status = bdb->del(bdb, &q, 0);
	if (status == 1)
		ret = -1;
	else if (status == 0)
		ret = 0;
	else
		ret = errno;
# endif /* DB_VERSION_CHECK(2,0,0) */

	/* surrender write-lock */
	if (fd != -1)
	{
# ifdef LOCK_UN
		status = flock(fd, LOCK_UN);
		if (status != 0)
		{
			db->db_status = status;
			return -1;
		}
# else /* LOCK_UN */
		struct flock l;

		l.l_start = 0;
		l.l_len = 0;
		l.l_type = F_UNLCK;
		l.l_whence = SEEK_SET;

		status = fcntl(fd, F_SETLKW, &l);
		if (status != 0)
		{
			db->db_status = status;
			return -1;
		}
# endif /* LOCK_UN */
	}

	if (db->db_lock != NULL)
		(void) pthread_mutex_unlock(db->db_lock);

	return ret;
#endif /* DKIMF_DB_TYPE_BDB */
}

/*
**  DKIMF_DB_PUT -- store a key/data pair in an open database
**
**  Parameters:
**  	db -- DB handle to use for searching
**  	buf -- pointer to key record
**  	buflen -- size of key (use strlen() if 0)
**  	outbuf -- data buffer
**  	outbuflen -- number of bytes at outbuf to use as data
**
**  Return value:
**  	0 -- operation successful
**	!0 -- error occurred; error code returned
*/

int
dkimf_db_put(DKIM_DB db, void *buf, size_t buflen,
             void *outbuf, size_t outbuflen)
{
#ifdef DKIMF_DB_TYPE_BDB
	DBT d;
	DBT q;
	int fd;
	int status;
	int ret;
	DB *bdb;
#endif /* DKIMF_DB_TYPE_BDB */

	assert(db != NULL);
	assert(buf != NULL);
	assert(outbuf != NULL);

	if (db->db_type == DKIMF_DB_TYPE_FILE ||
	    db->db_type == DKIMF_DB_TYPE_CSL || 
	    db->db_type == DKIMF_DB_TYPE_REFILE)
		return EINVAL;

#ifdef DKIMF_DB_TYPE_BDB
	bdb = (DB *) db->db_handle;

	memset(&d, 0, sizeof d);
	memset(&q, 0, sizeof q);

	d.data = outbuf;
	d.size = outbuflen;
#if DB_VERSION_CHECK(2,0,0)
	d.ulen = d.size;
	d.flags = DB_DBT_USERMEM;
#endif /* DB_VERSION_CHECK(2,0,0) */

	q.data = (char *) buf;
	q.size = (buflen == 0 ? strlen(q.data) : buflen);
#if DB_VERSION_CHECK(2,0,0)
	q.ulen = q.size;
	q.flags = DB_DBT_USERMEM;
#endif /* DB_VERSION_CHECK(2,0,0) */

	ret = 0;

	/* establish write-lock */
	fd = -1;
# if DB_VERSION_CHECK(2,0,0)
	status = bdb->fd(bdb, &fd);
# else /* DB_VERSION_CHECK(2,0,0) */
	status = 0;
	fd = bdb->fd(bdb);
# endif /* DB_VERSION_CHECK(2,0,0) */

	if (db->db_lock != NULL)
		(void) pthread_mutex_lock(db->db_lock);

	if (status == 0 && fd != -1)
	{
# ifdef LOCK_EX
		status = flock(fd, LOCK_EX);
		if (status != 0)
		{
			db->db_status = status;
			return -1;
		}
# else /* LOCK_EX */
		struct flock l;

		l.l_start = 0;
		l.l_len = 0;
		l.l_type = F_WRLCK;
		l.l_whence = SEEK_SET;

		status = fcntl(fd, F_SETLKW, &l);
		if (status != 0)
		{
			db->db_status = status;
			return -1;
		}
# endif /* LOCK_EX */
	}

# if DB_VERSION_CHECK(2,0,0)
	status = bdb->put(bdb, NULL, &q, &d, 0);
	if (status == 0)
		ret = 0;
	else
		ret = status;
# else /* DB_VERSION_CHECK(2,0,0) */
	status = bdb->put(bdb, &q, &d, 0);
	if (status == 1)
		ret = -1;
	else if (status == 0)
		ret = 0;
	else
		ret = errno;
# endif /* DB_VERSION_CHECK(2,0,0) */

	/* surrender write-lock */
	if (fd != -1)
	{
# ifdef LOCK_UN
		status = flock(fd, LOCK_UN);
		if (status != 0)
		{
			db->db_status = status;
			return -1;
		}
# else /* LOCK_UN */
		struct flock l;

		l.l_start = 0;
		l.l_len = 0;
		l.l_type = F_UNLCK;
		l.l_whence = SEEK_SET;

		status = fcntl(fd, F_SETLKW, &l);
		if (status != 0)
		{
			db->db_status = status;
			return -1;
		}
# endif /* LOCK_UN */
	}

	if (db->db_lock != NULL)
		(void) pthread_mutex_unlock(db->db_lock);

	return ret;
#endif /* DKIMF_DB_TYPE_BDB */
}

/*
**  DKIMF_DB_GET -- retrieve data from an open database
**
**  Parameters:
**  	db -- DB handle to use for searching
**  	buf -- pointer to the key
**  	buflen -- length of key (use strlen() if 0)
**  	outbuf -- output buffer
**  	outbuflen -- IN: number of bytes available at outbuf
**  	             OUT: number of bytes written to outbuf
**  	exists -- pointer to a "_Bool" updated to be TRUE if the record
**  	          was found, FALSE otherwise (may be NULL)
**
**  Return value:
**  	0 -- operation successful
**	!0 -- error occurred; error code returned
**
**  Notes:
**  	"buflen" is not used for csl, file or refile types.
*/

int
dkimf_db_get(DKIM_DB db, void *buf, size_t buflen,
             void *outbuf, size_t *outbuflen, _Bool *exists)
{
	int status;
	int ret;
	_Bool matched;

	assert(db != NULL);
	assert(buf != NULL);

	switch (db->db_type)
	{
	  case DKIMF_DB_TYPE_FILE:
	  case DKIMF_DB_TYPE_CSL:
	  {
		struct dkimf_db_list *list;

		for (list = (struct dkimf_db_list *) db->db_handle;
		     list != NULL;
		     list = list->db_list_next)
		{
			matched = FALSE;

			if ((db->db_flags & DKIMF_DB_FLAG_ICASE) == 0)
			{
				if (strcmp(buf, list->db_list_key) == 0)
					matched = TRUE;
			}
			else
			{
				if (strcasecmp(buf, list->db_list_key) == 0)
					matched = TRUE;
			}

			/* break out on single matches */
			if (matched &&
			    (db->db_flags & DKIMF_DB_FLAG_MATCHBOTH) == 0)
				break;

			/* double match with no value matches all */
			if (list->db_list_value == NULL)
				break;

			matched = FALSE;

			if ((db->db_flags & DKIMF_DB_FLAG_ICASE) == 0)
			{
				if (strcmp(buf, list->db_list_value) == 0)
					matched = TRUE;
			}
			else
			{
				if (strcasecmp(buf, list->db_list_value) == 0)
					matched = TRUE;
			}

			if (matched)
				break;
		}

		if (list == NULL)
		{
			if (exists != NULL)
				*exists = FALSE;
		}
		else
		{
			if (exists != NULL)
				*exists = TRUE;
			if (list->db_list_value != NULL && outbuf != NULL)
			{
				*outbuflen = strlcpy(outbuf,
				                     list->db_list_value,
				                     *outbuflen);
			}
		}

		return 0;
	  }

	  case DKIMF_DB_TYPE_REFILE:
	  {
		struct dkimf_db_relist *list;

		list = (struct dkimf_db_relist *) db->db_handle;

		while (list != NULL)
		{
			if (regexec(&list->db_relist_re, buf, 0, NULL, 0) == 0)
			{
				if (exists != NULL)
					*exists = TRUE;

				return 0;
			}

			list = list->db_relist_next;
		}

		if (exists != NULL)
			*exists = FALSE;

		return 0;
	  }

#ifdef DKIMF_DB_TYPE_BDB
	  case DKIMF_DB_TYPE_BDB:
	  {
		int fd;
		DB *bdb;
		DBT d;
		DBT q;

		bdb = (DB *) db->db_handle;

		memset(&d, 0, sizeof d);
		memset(&q, 0, sizeof q);
		q.data = (char *) buf;
		q.size = (buflen == 0 ? strlen(q.data) : buflen);

		ret = 0;

# if DB_VERSION_CHECK(2,0,0)
		d.flags = DB_DBT_USERMEM|DB_DBT_PARTIAL;
# endif /* DB_VERSION_CHECK(2,0,0) */
		d.data = outbuf;
		d.size = (outbuflen == NULL ? 0 : *outbuflen);

		/* establish read-lock */
		fd = -1;
# if DB_VERSION_CHECK(2,0,0)
		status = bdb->fd(bdb, &fd);
# else /* DB_VERSION_CHECK(2,0,0) */
		status = 0;
		fd = bdb->fd(bdb);
# endif /* DB_VERSION_CHECK(2,0,0) */

		/* XXX -- allow multiple readers? */
		if (db->db_lock != NULL)
			(void) pthread_mutex_lock(db->db_lock);

		if (status == 0 && fd != -1)
		{
# ifdef LOCK_SH
			status = flock(fd, LOCK_SH);
			if (status != 0)
			{
				db->db_status = status;
				return -1;
			}
# else /* LOCK_SH */
			struct flock l;

			l.l_start = 0;
			l.l_len = 0;
			l.l_type = F_RDLCK;
			l.l_whence = SEEK_SET;

			status = fcntl(fd, F_SETLKW, &l);
			if (status != 0)
			{
				db->db_status = status;
				return -1;
			}
# endif /* LOCK_SH */
		}

# if DB_VERSION_CHECK(2,0,0)
		status = bdb->get(bdb, NULL, &q, &d, 0);
		if (status == 0)
		{
			if (exists != NULL)
				*exists = TRUE;
		
			if (outbuflen != NULL)
				*outbuflen = d.size;

			ret = 0;
		}
		else if (status == DB_NOTFOUND)
		{
			if (exists != NULL)
				*exists = FALSE;
			ret = 0;
		}
		else
		{
			ret = status;
		}
# else /* DB_VERSION_CHECK(2,0,0) */
		status = bdb->get(bdb, &q, &d, 0);
		if (status == 1)
		{
			if (exists != NULL)
				*exists = FALSE;
			ret = 0;
		}
		else if (status == 0)
		{
			if (exists != NULL)
				*exists = TRUE;

			if (outbuflen != NULL)
				*outbuflen = d.size;

			ret = 0;
		}
		else
		{
			ret = errno;
		}
# endif /* DB_VERSION_CHECK(2,0,0) */

		/* surrender read-lock */
		if (fd != -1)
		{
# ifdef LOCK_SH
			status = flock(fd, LOCK_UN);
			if (status != 0)
			{
				db->db_status = status;
				return -1;
			}
# else /* LOCK_SH */
			struct flock l;

			l.l_start = 0;
			l.l_len = 0;
			l.l_type = F_UNLCK;
			l.l_whence = SEEK_SET;

			status = fcntl(fd, F_SETLKW, &l);
			if (status != 0)
			{
				db->db_status = status;
				return -1;
			}
# endif /* LOCK_SH */
		}

		if (db->db_lock != NULL)
			(void) pthread_mutex_unlock(db->db_lock);

		return ret;
	  }
#endif /* DKIMF_DB_TYPE_BDB */

	  default:
		assert(0);
	}

	/* NOTREACHED */
}

/*
**  DKIMF_DB_CLOSE -- close a DB handle
**
**  Parameters:
**  	db -- DB handle to shut down
**
**  Return value:
**  	None.
*/

void
dkimf_db_close(DKIM_DB db)
{
	assert(db != NULL);

	if (db->db_array != NULL)
	{
		int c;

		if ((db->db_iflags & DKIMF_DB_IFLAG_FREEARRAY) != 0)
		{
			for (c = 0; db->db_array != NULL; c++)
				free(db->db_array[c]);
		}
		free(db->db_array);
		db->db_array = NULL;
	}

	switch (db->db_type)
	{
	  case DKIMF_DB_TYPE_FILE:
	  case DKIMF_DB_TYPE_CSL:
		dkimf_db_list_free(db->db_handle);
		free(db);
		break;

	  case DKIMF_DB_TYPE_REFILE:
		dkimf_db_relist_free(db->db_handle);
		free(db);
		break;

#ifdef DKIMF_DB_TYPE_BDB
	  case DKIMF_DB_TYPE_BDB:
# if DB_VERSION_CHECK(2,0,0)
		if (db->db_cursor != NULL)
			((DBC *) (db->db_cursor))->c_close((DBC *) db->db_cursor);
# endif /* DB_VERSION_CHECK(2,0,0) */
		DKIMF_DBCLOSE((DB *) (db->db_handle));
		break;
#endif /* DKIMF_DB_TYPE_BDB */

	  default:
		assert(0);
	}

	/* NOTREACHED */
}

/*
**  DKIMF_DB_STRERROR -- obtain an error string
**
**  Parameters:
**  	db -- DKIM_DB handle of interest
**  	err -- error buffer
**  	errlen -- bytes available at "err"
**
**  Return value:
**  	Bytes written to "err".
*/

int
dkimf_db_strerror(DKIM_DB db, char *err, size_t errlen)
{
	assert(db != NULL);
	assert(err != NULL);

	switch (db->db_type)
	{
	  case DKIMF_DB_TYPE_FILE:
	  case DKIMF_DB_TYPE_CSL:
		return strlcpy(err, strerror(db->db_status), errlen);

	  case DKIMF_DB_TYPE_REFILE:
		return regerror(db->db_status, db->db_data, err, errlen);

#ifdef DKIMF_DB_TYPE_BDB
	  case DKIMF_DB_TYPE_BDB:
		return strlcpy(err, DB_STRERROR(db->db_status), errlen);
#endif /* DKIMF_DB_TYPE_BDB */

	  default:
		assert(0);
	}

	/* NOTREACHED */
}

/*
**  DKIMF_DB_WALK -- walk a database
**
**  Parameters:
**  	db -- database
**  	first -- get first record?
**  	key -- buffer to receive the key
**  	keylen -- bytes available at "key" (updated)
**  	data -- buffer to receive the data
**  	datalen -- bytes available at "data" (updated)
**
**  Return value:
**  	0 -- record returned
**  	1 -- no more records
**  	-1 -- error
*/

int
dkimf_db_walk(DKIM_DB db, _Bool first, void *key, size_t *keylen,
              void *data, size_t *datalen)
{
	assert(db != NULL);

	if (db->db_type == DKIMF_DB_TYPE_REFILE)
		return -1;

	switch (db->db_type)
	{
	  case DKIMF_DB_TYPE_CSL:
	  case DKIMF_DB_TYPE_FILE:
	  {
		size_t out;
		struct dkimf_db_list *list;

		if (first)
			list = (struct dkimf_db_list *) db->db_handle;
		else
			list = (struct dkimf_db_list *) db->db_cursor;

		if (list == NULL)
			return 1;

		if (key != NULL)
		{
			assert(keylen != NULL);

			*keylen = strlcpy(key, list->db_list_key, *keylen);
		}

		if (data != NULL)
		{
			assert(datalen != NULL);

			if (list->db_list_value != NULL)
			{
				*datalen = strlcpy(data, list->db_list_value,
				                   *datalen);
			}
		}

		list = list->db_list_next;
		db->db_cursor = list;

		return 0;
	  }

#ifdef DKIMF_DB_TYPE_BDB
	  case DKIMF_DB_TYPE_BDB:
	  {
		int status = 0;
		DB *bdb;
		DBT k;
		DBT d;
# if DB_VERSION_CHECK(2,0,0)
		DBC *dbc;
# endif /* DB_VERSION_CHECK(2,0,0) */

		bdb = (DB *) db->db_handle;

# if DB_VERSION_CHECK(2,0,0)
		/* establish a cursor if needed */
		dbc = db->db_cursor;
		if (dbc == NULL)
		{
			status = bdb->cursor(bdb, NULL, &dbc, 0);
			if (status != 0)
			{
				db->db_status = status;
				return -1;
			}
		}
# endif /* DB_VERSION_CHECK(2,0,0) */

		memset(&k, '\0', sizeof k);
		memset(&d, '\0', sizeof d);

# if DB_VERSION_CHECK(2,0,0)
		k.data = (void *) key;
		k.flags = DB_DBT_USERMEM;
		k.ulen = (keylen != NULL ? *keylen : 0);
# endif /* DB_VERSION_CHECK(2,0,0) */

# if DB_VERSION_CHECK(2,0,0)
		d.data = (void *) data;
		d.flags = DB_DBT_USERMEM;
		d.ulen = (datalen != NULL ? *datalen : 0);
# endif /* DB_VERSION_CHECK(2,0,0) */

# if DB_VERSION_CHECK(2,0,0)
		status = dbc->c_get(dbc, &k, &d, first ? DB_FIRST : DB_NEXT);
# else /* DB_VERSION_CHECK(2,0,0) */
		status = db->seq(db, &k, &d, first ? R_FIRST : R_NEXT);
# endif /* DB_VERSION_CHECK(2,0,0) */
		if (status == DB_NOTFOUND)
		{
			return 1;
		}
		else if (status != 0)
		{
			db->db_status = status;
			return -1;
		}
		else
		{
			if (keylen != NULL)
				*keylen = k.size;
			if (datalen != NULL)
				*datalen = d.size;

			return 0;
		}
	  }
#endif /* DKIMF_DB_TYPE_BDB */
	}
}

/*
**  DKIMF_DB_MKARRAY -- make a (char *) array of DB contents
**
**  Parameters:
**  	db -- a DKIM_DB handle
**  	a -- array (returned)
**
**  Return value:
**  	Length of the created array, or -1 on error/empty.
*/

int
dkimf_db_mkarray(DKIM_DB db, char ***a)
{
	char **out;

	assert(db != NULL);
	assert(a != NULL);

	if (db->db_type == DKIMF_DB_TYPE_REFILE)
		return -1;

#ifdef DKIMF_DB_TYPE_BDB
	if (db->db_type != DKIMF_DB_TYPE_BDB && db->db_nrecs == 0)
		return 0;

	if (db->db_type != DKIMF_DB_TYPE_BDB && db->db_array != NULL)
		return db->db_array;
#endif /* DKIMF_DB_TYPE_BDB */

	if (db->db_type == DKIMF_DB_TYPE_FILE ||
	    db->db_type == DKIMF_DB_TYPE_CSL)
	{
		int c;
		struct dkimf_db_list *cur;

		out = (char **) malloc(sizeof(char *) * (db->db_nrecs + 1));
		if (out != NULL)
		{
			cur = db->db_handle;
			for (c = 0; c < db->db_nrecs; c++)
			{
				out[c] = cur->db_list_key;
				cur = cur->db_list_next;
			}

			out[c] = NULL;
		}

		db->db_array = out;

		*a = out;

		return c;
	}
#ifdef DKIMF_DB_TYPE_BDB
	else if (db->db_type == DKIMF_DB_TYPE_BDB)	/* DKIMF_DB_TYPE_BDB */
	{
		int c;
		int nr = 0;
		int na = 0;
		int status;
		char keybuf[BUFRSZ + 1];

		if (db->db_array != NULL)
		{
			for (c = 0; db->db_array[c] != NULL; c++)
				free(db->db_array[c]);
			free(db->db_array);
			db->db_array = NULL;
		}

		status = 0;
		while (status == 0)
		{
			memset(keybuf, '\0', sizeof keybuf);

			status = dkimf_db_walk(db, (nr == 0),
			                       keybuf, sizeof keybuf - 1,
			                       NULL, NULL);

			if (nr == 0)
			{
				out = (char **) malloc(sizeof(char *) * DEFARRAYSZ);
				if (out == NULL)
					return -1;

				out[0] = strdup(keybuf);
				if (out[0] == NULL)
				{
					free(out);
					return -1;
				}

				na = DEFARRAYSZ;
				nr = 1;
				out[nr] = NULL;
			}
			else if (nr + 1 == na)
			{
				int newsz;
				char **newout;

				newsz = na * 2;

				newout = (char **) realloc(out, sizeof (char *) * newsz);
				if (newout == NULL)
				{
					for (c = 0; c < nr; c++)
						free(out[c]);
					free(out);
					return -1;
				}

				out[nr] = strdup(keybuf);
				if (out[nr] == NULL)
				{
					for (c = 0; c < nr; c++)
						free(out[c]);
					free(out);
					return -1;
				}

				nr++;
				na = newsz;
				out[nr] = NULL;
			}
		}

		if (status == -1)
		{
			for (c = 0; c < nr; c++)
				free(out[c]);
			free(out);
			return -1;
		}

		db->db_array = out;
		db->db_iflags |= DKIMF_DB_IFLAG_FREEARRAY;
		*a = out;
		return nr;
	}
#endif /* DKIMF_DB_TYPE_BDB */
}
