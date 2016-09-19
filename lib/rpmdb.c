/** \ingroup rpmdb dbi
 * \file lib/rpmdb.c
 */

#include "system.h"

#include <sys/file.h>
#include <utime.h>
#include <errno.h>

#ifndef	DYING	/* XXX already in "system.h" */
#include <fnmatch.h>
#endif

#include <regex.h>

#include <rpm/rpmtypes.h>
#include <rpm/rpmurl.h>
#include <rpm/rpmpgp.h>
#include <rpm/rpmpgp.h>
#include <rpm/rpmmacro.h>
#include <rpm/rpmsq.h>
#include <rpm/rpmstring.h>
#include <rpm/rpmfileutil.h>
#include <rpm/rpmds.h>			/* XXX isInstallPreReq macro only */
#include <rpm/rpmlog.h>
#include <rpm/rpmdb.h>
#include <rpm/rpmts.h>
#include <rpm/argv.h>

#include "lib/rpmchroot.h"
#include "lib/rpmdb_internal.h"
#include "lib/fprint.h"
#include "lib/header_internal.h"	/* XXX for headerSetInstance() */
#include "lib/backend/dbiset.h"
#include "debug.h"

#undef HASHTYPE
#undef HTKEYTYPE
#undef HTDATATYPE
#define HASHTYPE dbChk
#define HTKEYTYPE unsigned int
#define HTDATATYPE rpmRC
#include "lib/rpmhash.H"
#include "lib/rpmhash.C"
#undef HASHTYPE
#undef HTKEYTYPE
#undef HTDATATYPE

typedef rpmRC (*idxfunc)(dbiIndex dbi, dbiCursor dbc, const char *keyp, size_t keylen,
			 dbiIndexItem rec);

static rpmRC tag2index(dbiIndex dbi, rpmTagVal rpmtag,
		       unsigned int hdrNum, Header h,
		       idxfunc idxupdate);

static rpmRC indexPut(dbiIndex dbi, rpmTagVal rpmtag, unsigned int hdrNum, Header h);
static rpmdb rpmdbUnlink(rpmdb db);

static int buildIndexes(rpmdb db)
{
    int rc = 0;
    Header h;
    rpmdbMatchIterator mi;

    rc += rpmdbOpenAll(db);

    /* If the main db was just created, this is expected - dont whine */
    if (!(dbiFlags(db->db_pkgs) & DBI_CREATED)) {
	rpmlog(RPMLOG_WARNING,
	       _("Generating %d missing index(es), please wait...\n"),
	       db->db_buildindex);
    }

    /* Don't call us again */
    db->db_buildindex = 0;

    dbSetFSync(db, 0);

    dbCtrl(db, RPMDB_CTRL_LOCK_RW);

    mi = rpmdbInitIterator(db, RPMDBI_PACKAGES, NULL, 0);
    while ((h = rpmdbNextIterator(mi))) {
	unsigned int hdrNum = headerGetInstance(h);
	/* Build all secondary indexes which were created on open */
	for (int dbix = 0; dbix < db->db_ndbi; dbix++) {
	    dbiIndex dbi = db->db_indexes[dbix];
	    if (dbi && (dbiFlags(dbi) & DBI_CREATED)) {
		rc += indexPut(dbi, db->db_tags[dbix], hdrNum, h);
	    }
	}
    }
    rpmdbFreeIterator(mi);

    dbCtrl(db, DB_CTRL_INDEXSYNC);
    dbCtrl(db, DB_CTRL_UNLOCK_RW);

    dbSetFSync(db, !db->cfg.db_no_fsync);
    return rc;
}

static int uintCmp(unsigned int a, unsigned int b)
{
    return (a != b);
}

static unsigned int uintId(unsigned int a)
{
    return a;
}

/** \ingroup dbi
 * Return (newly allocated) integer of the epoch.
 * @param s		version string optionally containing epoch number
 * @retval version	only the version part of s
 * @return		epoch integer within the [0; UINT32_MAX] interval,
 *                      or -1 for no epoch
 */
static int64_t splitEpoch(const char *s, const char **version)
{
    int64_t e;
    char *end;
    int saveerrno = errno;

    *version = s;
    e = strtol(s, &end, 10);
    if (*end == ':' && e >= 0 && e <= UINT32_MAX) {
	*version = end + 1;
    } else {
	e = -1;
    }

    errno = saveerrno;
    return e;
}

static int pkgdbOpen(rpmdb db, int flags, dbiIndex *dbip)
{
    int rc = 0;
    dbiIndex dbi = NULL;

    if (db == NULL)
	return -1;

    /* Is this it already open ? */
    if ((dbi = db->db_pkgs) != NULL)
	goto exit;

    rc = dbiOpen(db, RPMDBI_PACKAGES, &dbi, flags);

    if (rc == 0) {
	int verifyonly = (flags & RPMDB_FLAG_VERIFYONLY);

	db->db_pkgs = dbi;
	/* Allocate header checking cache .. based on some random number */
	if (!verifyonly && (db->db_checked == NULL)) {
	    db->db_checked = dbChkCreate(567, uintId, uintCmp, NULL, NULL);
	}
	/* If primary got created, we can safely run without fsync */
	if ((!verifyonly && (dbiFlags(dbi) & DBI_CREATED)) || db->cfg.db_no_fsync) {
	    rpmlog(RPMLOG_DEBUG, "disabling fsync on database\n");
	    db->cfg.db_no_fsync = 1;
	    dbSetFSync(db, 0);
	}
    } else {
	rpmlog(RPMLOG_ERR, _("cannot open %s index using %s - %s (%d)\n"),
		   rpmTagGetName(RPMDBI_PACKAGES), db->db_descr,
		   (rc > 0 ? strerror(rc) : ""), rc);
    }

exit:
    if (rc == 0 && dbip)
	*dbip = dbi;

    return rc;
}

static int indexOpen(rpmdb db, rpmDbiTagVal rpmtag, int flags, dbiIndex *dbip)
{
    int dbix, rc = 0;
    dbiIndex dbi = NULL;

    if (db == NULL)
	return -1;

    for (dbix = 0; dbix < db->db_ndbi; dbix++) {
	if (rpmtag == db->db_tags[dbix])
	    break;
    }
    if (dbix >= db->db_ndbi)
	return -1;

    /* Is this index already open ? */
    if ((dbi = db->db_indexes[dbix]) != NULL)
	goto exit;

    rc = dbiOpen(db, rpmtag, &dbi, flags);

    if (rc == 0) {
	int verifyonly = (flags & RPMDB_FLAG_VERIFYONLY);
	int rebuild = (db->db_flags & RPMDB_FLAG_REBUILD);

	db->db_indexes[dbix] = dbi;
	if (!rebuild && !verifyonly && (dbiFlags(dbi) & DBI_CREATED)) {
	    rpmlog(RPMLOG_DEBUG, "index %s needs creating\n", dbiName(dbi));
	    db->db_buildindex++;
	    if (db->db_buildindex == 1) {
		buildIndexes(db);
	    }
	}
    } else {
	rpmlog(RPMLOG_ERR, _("cannot open %s index using %s - %s (%d)\n"),
		   rpmTagGetName(rpmtag), db->db_descr,
		   (rc > 0 ? strerror(rc) : ""), rc);
    }

exit:
    if (rc == 0 && dbip)
	*dbip = dbi;
    return rc;
}

static rpmRC indexGet(dbiIndex dbi, const char *keyp, size_t keylen,
		       dbiIndexSet *set)
{
    rpmRC rc = RPMRC_FAIL; /* assume failure */
    if (dbi != NULL) {
	dbiCursor dbc = dbiCursorInit(dbi, DBC_READ);

	if (keyp) {
	    if (keylen == 0)
		keylen = strlen(keyp);
	    rc = idxdbGet(dbi, dbc, keyp, keylen, set, DBC_NORMAL_SEARCH);
	} else {
	    do {
		rc = idxdbGet(dbi, dbc, NULL, 0, set, DBC_NORMAL_SEARCH);
	    } while (rc == RPMRC_OK);

	    /* If we got some results, not found is not an error */
	    if (rc == RPMRC_NOTFOUND && set != NULL)
		rc = RPMRC_OK;
	}

	dbiCursorFree(dbi, dbc);
    }
    return rc;
}

static rpmRC indexPrefixGet(dbiIndex dbi, const char *pfx, size_t plen,
			    dbiIndexSet *set)
{
    rpmRC rc = RPMRC_FAIL; /* assume failure */

    if (dbi != NULL && pfx) {
	dbiCursor dbc = dbiCursorInit(dbi, DBC_READ);

	if (plen == 0)
	    plen = strlen(pfx);
	rc = idxdbGet(dbi, dbc, pfx, plen, set, DBC_PREFIX_SEARCH);

	dbiCursorFree(dbi, dbc);
    }
    return rc;
}


typedef struct miRE_s {
    rpmTagVal		tag;		/*!< header tag */
    rpmMireMode		mode;		/*!< pattern match mode */
    char *		pattern;	/*!< pattern string */
    int			notmatch;	/*!< like "grep -v" */
    regex_t *		preg;		/*!< regex compiled pattern buffer */
    int			cflags;		/*!< regcomp(3) flags */
    int			eflags;		/*!< regexec(3) flags */
    int			fnflags;	/*!< fnmatch(3) flags */
} * miRE;

struct rpmdbMatchIterator_s {
    rpmdbMatchIterator	mi_next;
    rpmdb		mi_db;
    rpmDbiTagVal	mi_rpmtag;
    dbiIndexSet		mi_set;
    dbiCursor		mi_dbc;
    int			mi_setx;
    Header		mi_h;
    int			mi_sorted;
    int			mi_cflags;
    int			mi_modified;
    unsigned int	mi_prevoffset;	/* header instance (native endian) */
    unsigned int	mi_offset;	/* header instance (native endian) */
    unsigned int	mi_filenum;	/* tag element (native endian) */
    int			mi_nre;
    miRE		mi_re;
    rpmts		mi_ts;
    rpmRC (*mi_hdrchk) (rpmts ts, const void * uh, size_t uc, char ** msg);

};

struct rpmdbIndexIterator_s {
    rpmdbIndexIterator	ii_next;
    rpmdb		ii_db;
    dbiIndex		ii_dbi;
    rpmDbiTag		ii_rpmtag;
    dbiCursor		ii_dbc;
    dbiIndexSet		ii_set;
    unsigned int	*ii_hdrNums;
};

static rpmdb rpmdbRock;
static rpmdbMatchIterator rpmmiRock;
static rpmdbIndexIterator rpmiiRock;

int rpmdbCheckTerminate(int terminate)
{
    sigset_t newMask, oldMask;
    static int terminating = 0;

    if (terminating) return 0;

    (void) sigfillset(&newMask);		/* block all signals */
    (void) sigprocmask(SIG_BLOCK, &newMask, &oldMask);

    if (rpmsqIsCaught(SIGINT) > 0
     || rpmsqIsCaught(SIGQUIT) > 0
     || rpmsqIsCaught(SIGHUP) > 0
     || rpmsqIsCaught(SIGTERM) > 0
     || rpmsqIsCaught(SIGPIPE) > 0
     || terminate)
	terminating = 1;

    if (terminating) {
	rpmdb db;
	rpmdbMatchIterator mi;
	rpmdbIndexIterator ii;

	while ((mi = rpmmiRock) != NULL) {
	    rpmmiRock = mi->mi_next;
	    mi->mi_next = NULL;
	    rpmdbFreeIterator(mi);
	}

	while ((ii = rpmiiRock) != NULL) {
	    rpmiiRock = ii->ii_next;
	    ii->ii_next = NULL;
	    rpmdbIndexIteratorFree(ii);
	}

	while ((db = rpmdbRock) != NULL) {
	    rpmdbRock = db->db_next;
	    db->db_next = NULL;
	    (void) rpmdbClose(db);
	}
    }
    sigprocmask(SIG_SETMASK, &oldMask, NULL);
    return terminating;
}

int rpmdbCheckSignals(void)
{
    if (rpmdbCheckTerminate(0)) {
	rpmlog(RPMLOG_DEBUG, "Exiting on signal...\n");
	exit(EXIT_FAILURE);
    }
    return 0;
}

/**
 * Block all signals, returning previous signal mask.
 */
static int blockSignals(sigset_t * oldMask)
{
    sigset_t newMask;

    (void) sigfillset(&newMask);		/* block all signals */
    (void) sigprocmask(SIG_BLOCK, &newMask, oldMask);
    (void) sigdelset(&newMask, SIGINT);
    (void) sigdelset(&newMask, SIGQUIT);
    (void) sigdelset(&newMask, SIGHUP);
    (void) sigdelset(&newMask, SIGTERM);
    (void) sigdelset(&newMask, SIGPIPE);
    return sigprocmask(SIG_BLOCK, &newMask, NULL);
}

/**
 * Restore signal mask.
 */
static int unblockSignals(sigset_t * oldMask)
{
    (void) rpmdbCheckSignals();
    return sigprocmask(SIG_SETMASK, oldMask, NULL);
}

rpmop rpmdbOp(rpmdb rpmdb, rpmdbOpX opx)
{
    rpmop op = NULL;
    switch (opx) {
    case RPMDB_OP_DBGET:
	op = &rpmdb->db_getops;
	break;
    case RPMDB_OP_DBPUT:
	op = &rpmdb->db_putops;
	break;
    case RPMDB_OP_DBDEL:
	op = &rpmdb->db_delops;
	break;
    default:
	break;
    }
    return op;
}

const char *rpmdbHome(rpmdb db)
{
    const char *dbdir = NULL;
    if (db) {
	dbdir = rpmChrootDone() ? db->db_home : db->db_fullpath;
    }
    return dbdir;
}

int rpmdbOpenAll(rpmdb db)
{
    int rc = 0;

    if (db == NULL) return -2;

    rc = pkgdbOpen(db, db->db_flags, NULL);
    for (int dbix = 0; dbix < db->db_ndbi; dbix++) {
	rc += indexOpen(db, db->db_tags[dbix], db->db_flags, NULL);
    }
    return rc;
}

static int dbiForeach(dbiIndex *dbis, int ndbi,
		  int (*func) (dbiIndex, unsigned int), int del)
{
    int xx, rc = 0;
    for (int dbix = ndbi; --dbix >= 0; ) {
	if (dbis[dbix] == NULL)
	    continue;
	xx = func(dbis[dbix], 0);
	if (xx && rc == 0) rc = xx;
	if (del)
	    dbis[dbix] = NULL;
    }
    return rc;
}

int rpmdbClose(rpmdb db)
{
    rpmdb * prev, next;
    int rc = 0;

    if (db == NULL)
	goto exit;

    prev = &rpmdbRock;
    while ((next = *prev) != NULL && next != db)
	prev = &next->db_next;
    if (!next)
	goto exit;

    (void) rpmdbUnlink(db);

    if (db->nrefs > 0)
	goto exit;

    /* Always re-enable fsync on close of rw-database */
    if ((db->db_mode & O_ACCMODE) != O_RDONLY)
	dbSetFSync(db, 1);

    if (db->db_pkgs)
	rc = dbiClose(db->db_pkgs, 0);
    rc += dbiForeach(db->db_indexes, db->db_ndbi, dbiClose, 1);

    db->db_root = _free(db->db_root);
    db->db_home = _free(db->db_home);
    db->db_fullpath = _free(db->db_fullpath);
    db->db_checked = dbChkFree(db->db_checked);
    db->db_indexes = _free(db->db_indexes);
    db->db_descr = _free(db->db_descr);

    if (next) {
        *prev = next->db_next;
	next->db_next = NULL;
    }

    db = _free(db);

    if (rpmdbRock == NULL) {
	(void) rpmsqEnable(-SIGHUP, NULL);
	(void) rpmsqEnable(-SIGINT, NULL);
	(void) rpmsqEnable(-SIGTERM, NULL);
	(void) rpmsqEnable(-SIGQUIT, NULL);
	(void) rpmsqEnable(-SIGPIPE, NULL);
    }
exit:
    return rc;
}

static rpmdb newRpmdb(const char * root, const char * home,
		      int mode, int perms, int flags)
{
    rpmdb db = NULL;
    char * db_home = rpmGetPath((home && *home) ? home : "%{_dbpath}", NULL);

    static rpmDbiTag const dbiTags[] = {
	RPMDBI_NAME,
	RPMDBI_BASENAMES,
	RPMDBI_GROUP,
	RPMDBI_REQUIRENAME,
	RPMDBI_PROVIDENAME,
	RPMDBI_CONFLICTNAME,
	RPMDBI_OBSOLETENAME,
	RPMDBI_TRIGGERNAME,
	RPMDBI_DIRNAMES,
	RPMDBI_INSTALLTID,
	RPMDBI_SIGMD5,
	RPMDBI_SHA1HEADER,
	RPMDBI_FILETRIGGERNAME,
	RPMDBI_TRANSFILETRIGGERNAME,
	RPMDBI_RECOMMENDNAME,
	RPMDBI_SUGGESTNAME,
	RPMDBI_SUPPLEMENTNAME,
	RPMDBI_ENHANCENAME,
    };

    if (!(db_home && db_home[0] != '%')) {
	rpmlog(RPMLOG_ERR, _("no dbpath has been set\n"));
	free(db_home);
	return NULL;
    }

    db = xcalloc(sizeof(*db), 1);

    if (!(perms & 0600)) perms = 0644;	/* XXX sanity */

    db->db_mode = (mode >= 0) ? mode : 0;
    db->db_perms = (perms >= 0) ? perms : 0644;
    db->db_flags = (flags >= 0) ? flags : 0;

    db->db_home = db_home;
    db->db_root = rpmGetPath((root && *root) ? root : "/", NULL);
    db->db_fullpath = rpmGenPath(db->db_root, db->db_home, NULL);
    /* XXX remove environment after chrooted operations, for now... */
    db->db_remove_env = (!rstreq(db->db_root, "/") ? 1 : 0);
    db->db_tags = dbiTags;
    db->db_ndbi = sizeof(dbiTags) / sizeof(rpmDbiTag);
    db->db_indexes = xcalloc(db->db_ndbi, sizeof(*db->db_indexes));
    db->db_descr = xstrdup("unknown db");
    db->nrefs = 0;
    return rpmdbLink(db);
}

static int openDatabase(const char * prefix,
		const char * dbpath, rpmdb *dbp,
		int mode, int perms, int flags)
{
    rpmdb db;
    int rc;
    int justCheck = flags & RPMDB_FLAG_JUSTCHECK;

    if (dbp)
	*dbp = NULL;
    if ((mode & O_ACCMODE) == O_WRONLY) 
	return 1;

    db = newRpmdb(prefix, dbpath, mode, perms, flags);
    if (db == NULL)
	return 1;

    /* Try to ensure db home exists, error out if we can't even create */
    rc = rpmioMkpath(rpmdbHome(db), 0755, getuid(), getgid());
    if (rc == 0) {
	if (rpmdbRock == NULL) {
	    (void) rpmsqEnable(SIGHUP, NULL);
	    (void) rpmsqEnable(SIGINT, NULL);
	    (void) rpmsqEnable(SIGTERM, NULL);
	    (void) rpmsqEnable(SIGQUIT, NULL);
	    (void) rpmsqEnable(SIGPIPE, NULL);
	}

	/* Just the primary Packages database opened here */
	rc = pkgdbOpen(db, db->db_flags, NULL);
    }

    if (rc || justCheck || dbp == NULL)
	rpmdbClose(db);
    else {
	db->db_next = rpmdbRock;
	rpmdbRock = db;
        *dbp = db;
    }

    return rc;
}

static rpmdb rpmdbUnlink(rpmdb db)
{
    if (db)
	db->nrefs--;
    return NULL;
}

rpmdb rpmdbLink(rpmdb db)
{
    if (db)
	db->nrefs++;
    return db;
}

int rpmdbOpen (const char * prefix, rpmdb *dbp, int mode, int perms)
{
    return openDatabase(prefix, NULL, dbp, mode, perms, 0);
}

int rpmdbInit (const char * prefix, int perms)
{
    rpmdb db = NULL;
    int rc;

    rc = openDatabase(prefix, NULL, &db, (O_CREAT | O_RDWR), perms, 0);
    if (db != NULL) {
	int xx;
	xx = rpmdbOpenAll(db);
	if (xx && rc == 0) rc = xx;
	xx = rpmdbClose(db);
	if (xx && rc == 0) rc = xx;
	db = NULL;
    }
    return rc;
}

int rpmdbVerify(const char * prefix)
{
    rpmdb db = NULL;
    int rc = 0;

    rc = openDatabase(prefix, NULL, &db, O_RDONLY, 0644, RPMDB_FLAG_VERIFYONLY);

    if (db != NULL) {
	int xx;
	rc = rpmdbOpenAll(db);

	
	if (db->db_pkgs)
	    rc += dbiVerify(db->db_pkgs, 0);
	rc += dbiForeach(db->db_indexes, db->db_ndbi, dbiVerify, 0);

	xx = rpmdbClose(db);
	if (xx && rc == 0) rc = xx;
	db = NULL;
    }
    return rc;
}

Header rpmdbGetHeaderAt(rpmdb db, unsigned int offset)
{   
    rpmdbMatchIterator mi = rpmdbInitIterator(db, RPMDBI_PACKAGES,
					      &offset, sizeof(offset));
    Header h = headerLink(rpmdbNextIterator(mi));
    rpmdbFreeIterator(mi);
    return h;
}

/**
 * Find file matches in database.
 * @param db		rpm database
 * @param dbi		index database handle (always RPMDBI_BASENAMES)
 * @param filespec
 * @param usestate	take file state into account?
 * @retval matches
 * @return 		RPMRC_OK on match, RPMRC_NOMATCH or RPMRC_FAIL
 */
static rpmRC rpmdbFindByFile(rpmdb db, dbiIndex dbi, const char *filespec,
			   int usestate, dbiIndexSet * matches)
{
    char * dirName = NULL;
    const char * baseName;
    fingerPrintCache fpc = NULL;
    fingerPrint * fp1 = NULL;
    dbiIndexSet allMatches = NULL;
    unsigned int i;
    rpmRC rc = RPMRC_FAIL; /* assume error */

    *matches = NULL;
    if (filespec == NULL) return rc; /* nothing alloced yet */

    if ((baseName = strrchr(filespec, '/')) != NULL) {
	size_t len = baseName - filespec + 1;
	dirName = strncpy(xmalloc(len + 1), filespec, len);
	dirName[len] = '\0';
	baseName++;
    } else {
	dirName = xstrdup("");
	baseName = filespec;
    }
    if (baseName == NULL)
	goto exit;

    rc = indexGet(dbi, baseName, 0, &allMatches);

    if (rc || allMatches == NULL) goto exit;

    *matches = dbiIndexSetNew(0);
    fpc = fpCacheCreate(allMatches->count, NULL);
    fpLookup(fpc, dirName, baseName, &fp1);

    i = 0;
    while (i < allMatches->count) {
	struct rpmtd_s bn, dn, di, fs;
	const char ** baseNames, ** dirNames;
	uint32_t * dirIndexes;
	unsigned int offset = dbiIndexRecordOffset(allMatches, i);
	unsigned int prevoff;
	Header h = rpmdbGetHeaderAt(db, offset);

	if (h == NULL) {
	    i++;
	    continue;
	}

	headerGet(h, RPMTAG_BASENAMES, &bn, HEADERGET_MINMEM);
	headerGet(h, RPMTAG_DIRNAMES, &dn, HEADERGET_MINMEM);
	headerGet(h, RPMTAG_DIRINDEXES, &di, HEADERGET_MINMEM);
	baseNames = bn.data;
	dirNames = dn.data;
	dirIndexes = di.data;
	if (usestate)
	    headerGet(h, RPMTAG_FILESTATES, &fs, HEADERGET_MINMEM);

	do {
	    unsigned int num = dbiIndexRecordFileNumber(allMatches, i);
	    int skip = 0;

	    if (usestate) {
		rpmtdSetIndex(&fs, num);
		if (!RPMFILE_IS_INSTALLED(rpmtdGetNumber(&fs))) {
		    skip = 1;
		}
	    }

	    if (!skip) {
		const char *dirName = dirNames[dirIndexes[num]];
		if (fpLookupEquals(fpc, fp1, dirName, baseNames[num])) {
		    struct dbiIndexItem_s rec = { 
			.hdrNum = dbiIndexRecordOffset(allMatches, i),
			.tagNum = dbiIndexRecordFileNumber(allMatches, i),
		    };
		    dbiIndexSetAppend(*matches, &rec, 1, 0);
		}
	    }

	    prevoff = offset;
	    i++;
	    if (i < allMatches->count)
		offset = dbiIndexRecordOffset(allMatches, i);
	} while (i < allMatches->count && offset == prevoff);

	rpmtdFreeData(&bn);
	rpmtdFreeData(&dn);
	rpmtdFreeData(&di);
	if (usestate)
	    rpmtdFreeData(&fs);
	headerFree(h);
    }

    free(fp1);
    fpCacheFree(fpc);

    if ((*matches)->count == 0) {
	*matches = dbiIndexSetFree(*matches);
	rc = RPMRC_NOTFOUND;
    } else {
	rc = RPMRC_OK;
    }

exit:
    dbiIndexSetFree(allMatches);
    free(dirName);
    return rc;
}

int rpmdbCountPackages(rpmdb db, const char * name)
{
    int count = -1;
    dbiIndex dbi = NULL;

    if (name != NULL && indexOpen(db, RPMDBI_NAME, 0, &dbi) == 0) {
	dbiIndexSet matches = NULL;

	rpmRC rc = indexGet(dbi, name, strlen(name), &matches);

	if (rc == RPMRC_OK) {
	    count = dbiIndexSetCount(matches);
	} else {
	    count = (rc == RPMRC_NOTFOUND) ? 0 : -1;
	}
	dbiIndexSetFree(matches);
    }

    return count;
}

/**
 * Attempt partial matches on name[-version[-release]][.arch] strings.
 * @param db		rpmdb handle
 * @param dbi		index database
 * @param name		package name
 * @param epoch 	package epoch (-1 for any epoch)
 * @param version	package version (can be a pattern)
 * @param release	package release (can be a pattern)
 * @param arch		package arch (can be a pattern)
 * @retval matches	set of header instances that match
 * @return 		RPMRC_OK on match, RPMRC_NOMATCH or RPMRC_FAIL
 */
static rpmRC dbiFindMatches(rpmdb db, dbiIndex dbi,
		const char * name,
		int64_t epoch,
		const char * version,
		const char * release,
		const char * arch,
		dbiIndexSet * matches)
{
    unsigned int gotMatches = 0;
    rpmRC rc;
    unsigned int i;

    rc = indexGet(dbi, name, strlen(name), matches);

    /* No matches on the name, anything else wont match either */
    if (rc != RPMRC_OK)
	goto exit;
    
    /* If we got matches on name and nothing else was specified, we're done */
    if (epoch < 0 && version == NULL && release == NULL && arch == NULL)
	goto exit;

    /* Make sure the version and release match. */
    for (i = 0; i < dbiIndexSetCount(*matches); i++) {
	unsigned int recoff = dbiIndexRecordOffset(*matches, i);
	rpmdbMatchIterator mi;
	Header h;

	if (recoff == 0)
	    continue;

	mi = rpmdbInitIterator(db, RPMDBI_PACKAGES, &recoff, sizeof(recoff));

	/* Set iterator selectors for version/release if available. */
	if (version &&
	    rpmdbSetIteratorRE(mi, RPMTAG_VERSION, RPMMIRE_DEFAULT, version))
	{
	    rc = RPMRC_FAIL;
	    goto exit;
	}
	if (release &&
	    rpmdbSetIteratorRE(mi, RPMTAG_RELEASE, RPMMIRE_DEFAULT, release))
	{
	    rc = RPMRC_FAIL;
	    goto exit;
	}
	if (arch &&
	    rpmdbSetIteratorRE(mi, RPMTAG_ARCH, RPMMIRE_DEFAULT, arch))
	{
	    rc = RPMRC_FAIL;
	    goto exit;
	}

	h = rpmdbNextIterator(mi);

	if (epoch >= 0 && h) {
	    struct rpmtd_s td;
	    headerGet(h, RPMTAG_EPOCH, &td, HEADERGET_MINMEM);
	    if (epoch != rpmtdGetNumber(&td))
		h = NULL;
	    rpmtdFreeData(&td);
	}
	if (h)
	    (*matches)->recs[gotMatches++] = (*matches)->recs[i];
	else
	    (*matches)->recs[i].hdrNum = 0;
	rpmdbFreeIterator(mi);
    }

    if (gotMatches) {
	(*matches)->count = gotMatches;
	rc = RPMRC_OK;
    } else
	rc = RPMRC_NOTFOUND;

exit:
/* FIX: double indirection */
    if (rc && matches && *matches)
	*matches = dbiIndexSetFree(*matches);
    return rc;
}

/**
 * Lookup by name, name-version, and finally by name-version-release.
 * Both version and release can be patterns.
 * @todo Name must be an exact match, as name is a db key.
 * @param db		rpmdb handle
 * @param dbi		index database handle (always RPMDBI_NAME)
 * @param arg		name[-[epoch:]version[-release]] string
 * @param arglen	length of arg
 * @param arch		possible arch string (or NULL)
 * @retval matches	set of header instances that match
 * @return 		RPMRC_OK on match, RPMRC_NOMATCH or RPMRC_FAIL
 */
static rpmRC dbiFindByLabelArch(rpmdb db, dbiIndex dbi,
			    const char * arg, size_t arglen, const char *arch,
			    dbiIndexSet * matches)
{
    char localarg[arglen+1];
    int64_t epoch;
    const char * version;
    const char * release;
    char * s;
    char c;
    int brackets;
    rpmRC rc;

    if (arglen == 0) return RPMRC_NOTFOUND;

    strncpy(localarg, arg, arglen);
    localarg[arglen] = '\0';

    /* did they give us just a name? */
    rc = dbiFindMatches(db, dbi, localarg, -1, NULL, NULL, arch, matches);
    if (rc != RPMRC_NOTFOUND)
	goto exit;

    /* FIX: double indirection */
    *matches = dbiIndexSetFree(*matches);

    /* maybe a name-[epoch:]version ? */
    s = localarg + arglen;

    c = '\0';
    brackets = 0;
    for (s -= 1; s > localarg; s--) {
	switch (*s) {
	case '[':
	    brackets = 1;
	    break;
	case ']':
	    if (c != '[') brackets = 0;
	    break;
	}
	if (!brackets && c && *s == '-')
	    break;
	c = *s;
    }

   	/* FIX: *matches may be NULL. */
    if (s == localarg) {
	rc = RPMRC_NOTFOUND;
	goto exit;
    }

    *s = '\0';

    epoch = splitEpoch(s + 1, &version);
    rc = dbiFindMatches(db, dbi, localarg, epoch, version, NULL, arch, matches);
    if (rc != RPMRC_NOTFOUND) goto exit;

    /* FIX: double indirection */
    *matches = dbiIndexSetFree(*matches);

    /* how about name-[epoch:]version-release? */

    release = s + 1;

    c = '\0';
    brackets = 0;
    for (; s > localarg; s--) {
	switch (*s) {
	case '[':
	    brackets = 1;
	    break;
	case ']':
	    if (c != '[') brackets = 0;
	    break;
	}
	if (!brackets && c && *s == '-')
	    break;
	c = *s;
    }

    if (s == localarg) {
	rc = RPMRC_NOTFOUND;
	goto exit;
    }

    *s = '\0';
   	/* FIX: *matches may be NULL. */
    epoch = splitEpoch(s + 1, &version);
    rc = dbiFindMatches(db, dbi, localarg, epoch, version, release, arch, matches);
exit:
    return rc;
}

static rpmRC dbiFindByLabel(rpmdb db, dbiIndex dbi, const char * label,
			    dbiIndexSet * matches)
{
    const char *arch = NULL;
    /* First, try with label as it is */
    rpmRC rc = dbiFindByLabelArch(db, dbi, label, strlen(label), NULL, matches);

    /* If not found, retry with possible .arch specifier if there is one */
    if (rc == RPMRC_NOTFOUND && (arch = strrchr(label, '.')))
	rc = dbiFindByLabelArch(db, dbi, label, arch-label, arch+1, matches);

    return rc;
}

/**
 * Rewrite a header into packages (if necessary) and free the header.
 *   Note: this is called from a markReplacedFiles iteration, and *must*
 *   preserve the "join key" (i.e. offset) for the header.
 * @param mi		database iterator
 * @param dbi		index database handle
 * @return 		0 on success
 */
static int miFreeHeader(rpmdbMatchIterator mi, dbiIndex dbi)
{
    int rc = 0;

    if (mi == NULL || mi->mi_h == NULL)
	return 0;

    if (dbi && mi->mi_dbc && mi->mi_modified && mi->mi_prevoffset) {
	rpmRC rpmrc = RPMRC_NOTFOUND;
	unsigned int hdrLen = 0;
	unsigned char *hdrBlob = headerExport(mi->mi_h, &hdrLen);

	/* Check header digest/signature on blob export (if requested). */
	if (mi->mi_hdrchk && mi->mi_ts) {
	    char * msg = NULL;
	    int lvl;

	    rpmrc = (*mi->mi_hdrchk) (mi->mi_ts, hdrBlob, hdrLen, &msg);
	    lvl = (rpmrc == RPMRC_FAIL ? RPMLOG_ERR : RPMLOG_DEBUG);
	    rpmlog(lvl, "%s h#%8u %s",
		(rpmrc == RPMRC_FAIL ? _("miFreeHeader: skipping") : "write"),
			mi->mi_prevoffset, (msg ? msg : "\n"));
	    msg = _free(msg);
	}

	if (hdrBlob != NULL && rpmrc != RPMRC_FAIL) {
	    sigset_t signalMask;

	    blockSignals(&signalMask);
	    dbCtrl(mi->mi_db, DB_CTRL_LOCK_RW);
	    rc = pkgdbPut(dbi, mi->mi_dbc, mi->mi_prevoffset,
			  hdrBlob, hdrLen);
	    dbCtrl(mi->mi_db, DB_CTRL_INDEXSYNC);
	    dbCtrl(mi->mi_db, DB_CTRL_UNLOCK_RW);
	    unblockSignals(&signalMask);

	    if (rc) {
		rpmlog(RPMLOG_ERR,
			_("error(%d) storing record #%d into %s\n"),
			rc, mi->mi_prevoffset, dbiName(dbi));
	    }
	}
	free(hdrBlob);
    }

    mi->mi_h = headerFree(mi->mi_h);

    return rc;
}

rpmdbMatchIterator rpmdbFreeIterator(rpmdbMatchIterator mi)
{
    rpmdbMatchIterator * prev, next;
    dbiIndex dbi = NULL;
    int i;

    if (mi == NULL)
	return NULL;

    prev = &rpmmiRock;
    while ((next = *prev) != NULL && next != mi)
	prev = &next->mi_next;
    if (next) {
	*prev = next->mi_next;
	next->mi_next = NULL;
    } else
	return NULL;

    pkgdbOpen(mi->mi_db, 0, &dbi);

    miFreeHeader(mi, dbi);

    mi->mi_dbc = dbiCursorFree(dbi, mi->mi_dbc);

    if (mi->mi_re != NULL)
    for (i = 0; i < mi->mi_nre; i++) {
	miRE mire = mi->mi_re + i;
	mire->pattern = _free(mire->pattern);
	if (mire->preg != NULL) {
	    regfree(mire->preg);
	    mire->preg = _free(mire->preg);
	}
    }
    mi->mi_re = _free(mi->mi_re);

    mi->mi_set = dbiIndexSetFree(mi->mi_set);
    rpmdbClose(mi->mi_db);
    mi->mi_ts = rpmtsFree(mi->mi_ts);

    mi = _free(mi);

    (void) rpmdbCheckSignals();

    return NULL;
}

unsigned int rpmdbGetIteratorOffset(rpmdbMatchIterator mi)
{
    return (mi ? mi->mi_offset : 0);
}

unsigned int rpmdbGetIteratorFileNum(rpmdbMatchIterator mi)
{
    return (mi ? mi->mi_filenum : 0);
}

int rpmdbGetIteratorCount(rpmdbMatchIterator mi)
{
    return (mi && mi->mi_set ?  mi->mi_set->count : 0);
}

int rpmdbGetIteratorIndex(rpmdbMatchIterator mi)
{
    return (mi ? mi->mi_setx : 0);
}

void rpmdbSetIteratorIndex(rpmdbMatchIterator mi, unsigned int ix)
{
    if (mi)
	mi->mi_setx = ix;
}

unsigned int rpmdbGetIteratorOffsetFor(rpmdbMatchIterator mi, unsigned int ix)
{
    if (mi && mi->mi_set && ix < mi->mi_set->count)
	return mi->mi_set->recs[ix].hdrNum;
    return 0;
}

/**
 * Return pattern match.
 * @param mire		match iterator regex
 * @param val		value to match
 * @return		0 if pattern matches, >0 on nomatch, <0 on error
 */
static int miregexec(miRE mire, const char * val)
{
    int rc = 0;

    switch (mire->mode) {
    case RPMMIRE_STRCMP:
	rc = (!rstreq(mire->pattern, val));
	break;
    case RPMMIRE_DEFAULT:
    case RPMMIRE_REGEX:
	rc = regexec(mire->preg, val, 0, NULL, mire->eflags);
	if (rc && rc != REG_NOMATCH) {
	    char msg[256];
	    (void) regerror(rc, mire->preg, msg, sizeof(msg)-1);
	    msg[sizeof(msg)-1] = '\0';
	    rpmlog(RPMLOG_ERR, _("%s: regexec failed: %s\n"),
			mire->pattern, msg);
	    rc = -1;
	}
	break;
    case RPMMIRE_GLOB:
	rc = fnmatch(mire->pattern, val, mire->fnflags);
	if (rc && rc != FNM_NOMATCH)
	    rc = -1;
	break;
    default:
	rc = -1;
	break;
    }

    return rc;
}

/**
 * Compare iterator selectors by rpm tag (qsort/bsearch).
 * @param a		1st iterator selector
 * @param b		2nd iterator selector
 * @return		result of comparison
 */
static int mireCmp(const void * a, const void * b)
{
    const miRE mireA = (const miRE) a;
    const miRE mireB = (const miRE) b;
    return (mireA->tag - mireB->tag);
}

/**
 * Copy pattern, escaping for appropriate mode.
 * @param tag		rpm tag
 * @retval modep	type of pattern match
 * @param pattern	pattern to duplicate
 * @return		duplicated pattern
 */
static char * mireDup(rpmTagVal tag, rpmMireMode *modep,
			const char * pattern)
{
    const char * s;
    char * pat;
    char * t;
    int brackets;
    size_t nb;
    int c;

    switch (*modep) {
    default:
    case RPMMIRE_DEFAULT:
	if (tag == RPMTAG_DIRNAMES || tag == RPMTAG_BASENAMES) {
	    *modep = RPMMIRE_GLOB;
	    pat = xstrdup(pattern);
	    break;
	}

	nb = strlen(pattern) + sizeof("^$");

	/* Find no. of bytes needed for pattern. */
	/* periods and plusses are escaped, splats become '.*' */
	c = '\0';
	brackets = 0;
	for (s = pattern; *s != '\0'; s++) {
	    switch (*s) {
	    case '.':
	    case '+':
	    case '*':
		if (!brackets) nb++;
		break;
	    case '\\':
		s++;
		break;
	    case '[':
		brackets = 1;
		break;
	    case ']':
		if (c != '[') brackets = 0;
		break;
	    }
	    c = *s;
	}

	pat = t = xmalloc(nb);

	if (pattern[0] != '^') *t++ = '^';

	/* Copy pattern, escaping periods, prefixing splats with period. */
	c = '\0';
	brackets = 0;
	for (s = pattern; *s != '\0'; s++, t++) {
	    switch (*s) {
	    case '.':
	    case '+':
		if (!brackets) *t++ = '\\';
		break;
	    case '*':
		if (!brackets) *t++ = '.';
		break;
	    case '\\':
		*t++ = *s++;
		break;
	    case '[':
		brackets = 1;
		break;
	    case ']':
		if (c != '[') brackets = 0;
		break;
	    }
	    c = *t = *s;
	}

	if (s > pattern && s[-1] != '$') *t++ = '$';
	*t = '\0';
	*modep = RPMMIRE_REGEX;
	break;
    case RPMMIRE_STRCMP:
    case RPMMIRE_REGEX:
    case RPMMIRE_GLOB:
	pat = xstrdup(pattern);
	break;
    }

    return pat;
}

int rpmdbSetIteratorRE(rpmdbMatchIterator mi, rpmTagVal tag,
		rpmMireMode mode, const char * pattern)
{
    static rpmMireMode defmode = (rpmMireMode)-1;
    miRE mire = NULL;
    char * allpat = NULL;
    int notmatch = 0;
    regex_t * preg = NULL;
    int cflags = 0;
    int eflags = 0;
    int fnflags = 0;
    int rc = 0;

    if (defmode == (rpmMireMode)-1) {
	char *t = rpmExpand("%{?_query_selector_match}", NULL);

	if (*t == '\0' || rstreq(t, "default"))
	    defmode = RPMMIRE_DEFAULT;
	else if (rstreq(t, "strcmp"))
	    defmode = RPMMIRE_STRCMP;
	else if (rstreq(t, "regex"))
	    defmode = RPMMIRE_REGEX;
	else if (rstreq(t, "glob"))
	    defmode = RPMMIRE_GLOB;
	else
	    defmode = RPMMIRE_DEFAULT;
	free(t);
     }

    if (mi == NULL || pattern == NULL)
	return rc;

    /* Leading '!' inverts pattern match sense, like "grep -v". */
    if (*pattern == '!') {
	notmatch = 1;
	pattern++;
    }

    allpat = mireDup(tag, &mode, pattern);

    if (mode == RPMMIRE_DEFAULT)
	mode = defmode;

    switch (mode) {
    case RPMMIRE_DEFAULT:
    case RPMMIRE_STRCMP:
	break;
    case RPMMIRE_REGEX:
	preg = xcalloc(1, sizeof(*preg));
	cflags = (REG_EXTENDED | REG_NOSUB);
	rc = regcomp(preg, allpat, cflags);
	if (rc) {
	    char msg[256];
	    (void) regerror(rc, preg, msg, sizeof(msg)-1);
	    msg[sizeof(msg)-1] = '\0';
	    rpmlog(RPMLOG_ERR, _("%s: regcomp failed: %s\n"), allpat, msg);
	}
	break;
    case RPMMIRE_GLOB:
	fnflags = FNM_PATHNAME | FNM_PERIOD;
	break;
    default:
	rc = -1;
	break;
    }

    if (rc) {
	/* FIX: mire has kept values */
	allpat = _free(allpat);
	if (preg) {
	    regfree(preg);
	    preg = _free(preg);
	}
	return rc;
    }

    mi->mi_re = xrealloc(mi->mi_re, (mi->mi_nre + 1) * sizeof(*mi->mi_re));
    mire = mi->mi_re + mi->mi_nre;
    mi->mi_nre++;
    
    mire->tag = tag;
    mire->mode = mode;
    mire->pattern = allpat;
    mire->notmatch = notmatch;
    mire->preg = preg;
    mire->cflags = cflags;
    mire->eflags = eflags;
    mire->fnflags = fnflags;

    if (mi->mi_nre > 1)
	qsort(mi->mi_re, mi->mi_nre, sizeof(*mi->mi_re), mireCmp);

    return rc;
}

/**
 * Return iterator selector match.
 * @param mi		rpm database iterator
 * @return		1 if header should be skipped
 */
static int mireSkip (const rpmdbMatchIterator mi)
{
    miRE mire;
    uint32_t zero = 0;
    int ntags = 0;
    int nmatches = 0;
    int rc;

    if (mi->mi_h == NULL)	/* XXX can't happen */
	return 0;

    /*
     * Apply tag tests, implicitly "||" for multiple patterns/values of a
     * single tag, implicitly "&&" between multiple tag patterns.
     */
    if ((mire = mi->mi_re) != NULL)
    for (int i = 0; i < mi->mi_nre; i++, mire++) {
	int anymatch;
	struct rpmtd_s td;

	if (!headerGet(mi->mi_h, mire->tag, &td, HEADERGET_MINMEM)) {
	    if (mire->tag != RPMTAG_EPOCH) {
		ntags++;
		continue;
	    }
	    /* "is package already installed" checks rely on this behavior */
	    td.count = 1;
	    td.type = RPM_INT32_TYPE;
	    td.data = &zero;
	}

	anymatch = 0;		/* no matches yet */
	while (1) {
	    rpmtdInit(&td);
	    while (rpmtdNext(&td) >= 0) {
	    	char *str = rpmtdFormat(&td, RPMTD_FORMAT_STRING, NULL);
		if (str) {
		    rc = miregexec(mire, str);
		    if ((!rc && !mire->notmatch) || (rc && mire->notmatch))
			anymatch++;
		    free(str);
		}
	    }
	    if ((i+1) < mi->mi_nre && mire[0].tag == mire[1].tag) {
		i++;
		mire++;
		continue;
	    }
	    break;
	}
	rpmtdFreeData(&td);

	ntags++;
	if (anymatch)
	    nmatches++;
    }

    return (ntags == nmatches ? 0 : 1);
}

int rpmdbSetIteratorRewrite(rpmdbMatchIterator mi, int rewrite)
{
    int rc;
    if (mi == NULL)
	return 0;
    rc = (mi->mi_cflags & DBC_WRITE) ? 1 : 0;
    if (rewrite)
	mi->mi_cflags |= DBC_WRITE;
    else
	mi->mi_cflags &= ~DBC_WRITE;
    return rc;
}

int rpmdbSetIteratorModified(rpmdbMatchIterator mi, int modified)
{
    int rc;
    if (mi == NULL)
	return 0;
    rc = mi->mi_modified;
    mi->mi_modified = modified;
    return rc;
}

int rpmdbSetHdrChk(rpmdbMatchIterator mi, rpmts ts,
	rpmRC (*hdrchk) (rpmts ts, const void *uh, size_t uc, char ** msg))
{
    int rc = 0;
    if (mi == NULL)
	return 0;
    mi->mi_ts = rpmtsLink(ts);
    mi->mi_hdrchk = hdrchk;
    return rc;
}

static rpmRC miVerifyHeader(rpmdbMatchIterator mi, const void *uh, size_t uhlen)
{
    rpmRC rpmrc = RPMRC_NOTFOUND;

    if (!(mi->mi_hdrchk && mi->mi_ts))
	return rpmrc;

    /* Don't bother re-checking a previously read header. */
    if (mi->mi_db->db_checked) {
	rpmRC *res;
	if (dbChkGetEntry(mi->mi_db->db_checked, mi->mi_offset,
			  &res, NULL, NULL)) {
	    rpmrc = res[0];
	}
    }

    /* If blob is unchecked, check blob import consistency now. */
    if (rpmrc != RPMRC_OK) {
	char * msg = NULL;
	int lvl;

	rpmrc = (*mi->mi_hdrchk) (mi->mi_ts, uh, uhlen, &msg);
	lvl = (rpmrc == RPMRC_FAIL ? RPMLOG_ERR : RPMLOG_DEBUG);
	rpmlog(lvl, "%s h#%8u %s\n",
	    (rpmrc == RPMRC_FAIL ? _("rpmdbNextIterator: skipping") : " read"),
		    mi->mi_offset, (msg ? msg : ""));
	msg = _free(msg);

	/* Mark header checked. */
	if (mi->mi_db && mi->mi_db->db_checked) {
	    dbChkAddEntry(mi->mi_db->db_checked, mi->mi_offset, rpmrc);
	}
    }
    return rpmrc;
}

/* FIX: mi->mi_key.data may be NULL */
Header rpmdbNextIterator(rpmdbMatchIterator mi)
{
    dbiIndex dbi = NULL;
    unsigned char * uh;
    unsigned int uhlen;
    int rc;
    headerImportFlags importFlags = HEADERIMPORT_FAST;

    if (mi == NULL)
	return NULL;

    if (pkgdbOpen(mi->mi_db, 0, &dbi))
	return NULL;

#if defined(_USE_COPY_LOAD)
    importFlags |= HEADERIMPORT_COPY;
#endif
    /*
     * Cursors are per-iterator, not per-dbi, so get a cursor for the
     * iterator on 1st call. If the iteration is to rewrite headers,
     * then the cursor needs to marked with DBC_WRITE as well.
     */
    if (mi->mi_dbc == NULL)
	mi->mi_dbc = dbiCursorInit(dbi, mi->mi_cflags);

top:
    uh = NULL;
    uhlen = 0;

    do {
	if (mi->mi_set) {
	    if (!(mi->mi_setx < mi->mi_set->count))
		return NULL;
	    mi->mi_offset = dbiIndexRecordOffset(mi->mi_set, mi->mi_setx);
	    mi->mi_filenum = dbiIndexRecordFileNumber(mi->mi_set, mi->mi_setx);
	} else {
	    rc = pkgdbGet(dbi, mi->mi_dbc, 0, &uh, &uhlen);
	    if (rc == 0)
		mi->mi_offset = pkgdbKey(dbi, mi->mi_dbc);

	    /* Terminate on error or end of keys */
	    if (rc || (mi->mi_setx && mi->mi_offset == 0))
		return NULL;
	}
	mi->mi_setx++;
    } while (mi->mi_offset == 0);

    /* If next header is identical, return it now. */
    if (mi->mi_prevoffset && mi->mi_offset == mi->mi_prevoffset)
	return mi->mi_h;

    /* Retrieve next header blob for index iterator. */
    if (uh == NULL) {
	rc = pkgdbGet(dbi, mi->mi_dbc, mi->mi_offset, &uh, &uhlen);
	if (rc)
	    return NULL;
    }

    /* Rewrite current header (if necessary) and unlink. */
    miFreeHeader(mi, dbi);

    /* Is this the end of the iteration? */
    if (uh == NULL)
	return NULL;

    /* Verify header if enabled, skip damaged and inconsistent headers */
    if (miVerifyHeader(mi, uh, uhlen) == RPMRC_FAIL) {
	goto top;
    }

    /* Did the header blob load correctly? */
    mi->mi_h = headerImport(uh, uhlen, importFlags);
    if (mi->mi_h == NULL || !headerIsEntry(mi->mi_h, RPMTAG_NAME)) {
	rpmlog(RPMLOG_ERR,
		_("rpmdb: damaged header #%u retrieved -- skipping.\n"),
		mi->mi_offset);
	goto top;
    }

    /*
     * Skip this header if iterator selector (if any) doesn't match.
     */
    if (mireSkip(mi)) {
	goto top;
    }
    headerSetInstance(mi->mi_h, mi->mi_offset);

    mi->mi_prevoffset = mi->mi_offset;
    mi->mi_modified = 0;

    return mi->mi_h;
}

/** \ingroup rpmdb
 * sort the iterator by (recnum, filenum)
 * Return database iterator.
 * @param mi		rpm database iterator
 */
void rpmdbSortIterator(rpmdbMatchIterator mi)
{
    if (mi && mi->mi_set) {
	dbiIndexSetSort(mi->mi_set);
	mi->mi_sorted = 1;
    }
}

void rpmdbUniqIterator(rpmdbMatchIterator mi)
{
    if (mi && mi->mi_set) {
	dbiIndexSetUniq(mi->mi_set, mi->mi_sorted);
    }
}

int rpmdbExtendIterator(rpmdbMatchIterator mi,
			const void * keyp, size_t keylen)
{
    dbiIndex dbi = NULL;
    dbiIndexSet set = NULL;
    int rc = 1; /* assume failure */

    if (mi == NULL || keyp == NULL)
	return rc;

    rc = indexOpen(mi->mi_db, mi->mi_rpmtag, 0, &dbi);

    if (rc == 0 && indexGet(dbi, keyp, keylen, &set) == RPMRC_OK) {
	if (mi->mi_set == NULL) {
	    mi->mi_set = set;
	} else {
	    dbiIndexSetAppendSet(mi->mi_set, set, 0);
	    dbiIndexSetFree(set);
	}
	rc = 0;
    }

    return rc;
}

int rpmdbFilterIterator(rpmdbMatchIterator mi, packageHash hdrNums, int neg)
{
    if (mi == NULL || hdrNums == NULL)
	return 1;

    if (!mi->mi_set)
	return 0;

    if (packageHashNumKeys(hdrNums) == 0) {
	if (!neg)
	    mi->mi_set->count = 0;
	return 0;
    }

    unsigned int from;
    unsigned int to = 0;
    unsigned int num = mi->mi_set->count;
    int cond;

    assert(mi->mi_set->count > 0);

    for (from = 0; from < num; from++) {
	cond = !packageHashHasEntry(hdrNums, mi->mi_set->recs[from].hdrNum);
	cond = neg ? !cond : cond;
	if (cond) {
	    mi->mi_set->count--;
	    continue;
	}
	if (from != to)
	    mi->mi_set->recs[to] = mi->mi_set->recs[from]; /* structure assignment */
	to++;
    }
    return 0;
}

int rpmdbPruneIterator(rpmdbMatchIterator mi, packageHash hdrNums)
{
    if (packageHashNumKeys(hdrNums) <= 0)
	return 1;

    return rpmdbFilterIterator(mi, hdrNums, 1);
}


int rpmdbAppendIterator(rpmdbMatchIterator mi,
			const unsigned int * hdrNums, unsigned int nHdrNums)
{
    if (mi == NULL || hdrNums == NULL || nHdrNums == 0)
	return 1;

    if (mi->mi_set == NULL)
	mi->mi_set = dbiIndexSetNew(nHdrNums);

    for (unsigned int i = 0; i < nHdrNums; i++) {
	struct dbiIndexItem_s rec = { .hdrNum = hdrNums[i], .tagNum = 0 };
	dbiIndexSetAppend(mi->mi_set, &rec, 1, 0);
    }
    return 0;
}

rpmdbMatchIterator rpmdbNewIterator(rpmdb db, rpmDbiTagVal dbitag)
{
    rpmdbMatchIterator mi = NULL;

    if (dbitag == RPMDBI_PACKAGES) {
	if (pkgdbOpen(db, 0, NULL))
	    return NULL;
    } else {
	if (indexOpen(db, dbitag, 0, NULL))
	    return NULL;
    }

    mi = xcalloc(1, sizeof(*mi));
    mi->mi_set = NULL;
    mi->mi_db = rpmdbLink(db);
    mi->mi_rpmtag = dbitag;

    mi->mi_dbc = NULL;
    mi->mi_setx = 0;
    mi->mi_h = NULL;
    mi->mi_sorted = 0;
    mi->mi_cflags = 0;
    mi->mi_modified = 0;
    mi->mi_prevoffset = 0;
    mi->mi_offset = 0;
    mi->mi_filenum = 0;
    mi->mi_nre = 0;
    mi->mi_re = NULL;

    mi->mi_ts = NULL;
    mi->mi_hdrchk = NULL;

    /* Chain cursors for teardown on abnormal exit. */
    mi->mi_next = rpmmiRock;
    rpmmiRock = mi;

    return mi;
};

static rpmdbMatchIterator pkgdbIterInit(rpmdb db,
				    const unsigned int * keyp, size_t keylen)
{
    rpmdbMatchIterator mi = NULL;
    rpmDbiTagVal dbtag = RPMDBI_PACKAGES;
    dbiIndex pkgs = NULL;

    /* Require a sane keylen if one is specified */
    if (keyp && keylen != sizeof(*keyp))
	return NULL;

    if (pkgdbOpen(db, 0, &pkgs) == 0) {
	mi = rpmdbNewIterator(db, dbtag);
	if (keyp)
	    rpmdbAppendIterator(mi, keyp, 1);
    }
    return mi;
}

static rpmdbMatchIterator indexIterInit(rpmdb db, rpmDbiTagVal rpmtag,
				        const void * keyp, size_t keylen)
{
    rpmdbMatchIterator mi = NULL;
    rpmDbiTagVal dbtag = rpmtag;
    dbiIndex dbi = NULL;
    dbiIndexSet set = NULL;

    /* Fixup the physical index for our pseudo indexes */
    if (rpmtag == RPMDBI_LABEL) {
	dbtag = RPMDBI_NAME;
    } else if (rpmtag == RPMDBI_INSTFILENAMES) {
	dbtag = RPMDBI_BASENAMES;
    }

    if (indexOpen(db, dbtag, 0, &dbi) == 0) {
	int rc = 0;

        if (keyp) {
            if (rpmtag == RPMDBI_LABEL) {
                rc = dbiFindByLabel(db, dbi, keyp, &set);
            } else if (rpmtag == RPMDBI_BASENAMES) {
                rc = rpmdbFindByFile(db, dbi, keyp, 0, &set);
            } else if (rpmtag == RPMDBI_INSTFILENAMES) {
                rc = rpmdbFindByFile(db, dbi, keyp, 1, &set);
            } else {
		rc = indexGet(dbi, keyp, keylen, &set);
	    }
	} else {
            /* get all entries from index */
	    rc = indexGet(dbi, NULL, 0, &set);
        }

	if (rc)	{	/* error/not found */
	    set = dbiIndexSetFree(set);
	} else {
	    mi = rpmdbNewIterator(db, dbtag);
	    mi->mi_set = set;

	    if (keyp) {
		rpmdbSortIterator(mi);
	    }
	}
    }
    
    return mi;
}

rpmdbMatchIterator rpmdbInitIterator(rpmdb db, rpmDbiTagVal rpmtag,
		const void * keyp, size_t keylen)
{
    rpmdbMatchIterator mi = NULL;

    if (db != NULL) {
	(void) rpmdbCheckSignals();

	if (rpmtag == RPMDBI_PACKAGES)
	    mi = pkgdbIterInit(db, keyp, keylen);
	else
	    mi = indexIterInit(db, rpmtag, keyp, keylen);
    }

    return mi;
}

rpmdbMatchIterator rpmdbInitPrefixIterator(rpmdb db, rpmDbiTagVal rpmtag,
					    const void * pfx, size_t plen)
{
    rpmdbMatchIterator mi = NULL;
    dbiIndexSet set = NULL;
    dbiIndex dbi = NULL;
    rpmDbiTagVal dbtag = rpmtag;

    if (!pfx)
	return NULL;

    if (db != NULL && rpmtag != RPMDBI_PACKAGES) {
	(void) rpmdbCheckSignals();


	if (indexOpen(db, dbtag, 0, &dbi) == 0) {
	    int rc = 0;

	    rc = indexPrefixGet(dbi, pfx, plen, &set);

	    if (rc)	{
		set = dbiIndexSetFree(set);
	    } else {
		mi = rpmdbNewIterator(db, dbtag);
		mi->mi_set = set;
		rpmdbSortIterator(mi);
	    }
	}

    }

    return mi;
}

/*
 * Convert current tag data to db key
 * @param tagdata	Tag data container
 * @retval keylen	Length of key
 * @return 		Pointer to key value or NULL to signal skip 
 */
static const void * td2key(rpmtd tagdata, unsigned int *keylen) 
{
    const void * data = NULL;
    unsigned int size = 0;
    const char *str = NULL;

    switch (rpmtdType(tagdata)) {
    case RPM_CHAR_TYPE:
    case RPM_INT8_TYPE:
	size = sizeof(uint8_t);
	data = rpmtdGetChar(tagdata);
	break;
    case RPM_INT16_TYPE:
	size = sizeof(uint16_t);
	data = rpmtdGetUint16(tagdata);
	break;
    case RPM_INT32_TYPE:
	size = sizeof(uint32_t);
	data = rpmtdGetUint32(tagdata);
	break;
    case RPM_INT64_TYPE:
	size = sizeof(uint64_t);
	data = rpmtdGetUint64(tagdata);
	break;
    case RPM_BIN_TYPE:
	size = tagdata->count;
	data = tagdata->data;
	break;
    case RPM_STRING_TYPE:
    case RPM_I18NSTRING_TYPE:
    case RPM_STRING_ARRAY_TYPE:
	str = rpmtdGetString(tagdata);
	if (str) {
	    size = strlen(str);
	    data = str;
	}
	break;
    default:
	break;
    }

    if (data && keylen)
	*keylen = size;

    return data;
}
/*
 * rpmdbIndexIterator
 */

rpmdbIndexIterator rpmdbIndexIteratorInit(rpmdb db, rpmDbiTag rpmtag)
{
    rpmdbIndexIterator ii;
    dbiIndex dbi = NULL;

    if (db == NULL)
	return NULL;

    (void) rpmdbCheckSignals();

    if (indexOpen(db, rpmtag, 0, &dbi))
	return NULL;

    /* Chain cursors for teardown on abnormal exit. */
    ii = xcalloc(1, sizeof(*ii));
    ii->ii_next = rpmiiRock;
    rpmiiRock = ii;

    ii->ii_db = rpmdbLink(db);
    ii->ii_rpmtag = rpmtag;
    ii->ii_dbi = dbi;
    ii->ii_set = NULL;

    return ii;
}

int rpmdbIndexIteratorNext(rpmdbIndexIterator ii, const void ** key, size_t * keylen)
{
    int rc;
    unsigned int iikeylen = 0; /* argh, size_t vs uint pointer... */

    if (ii == NULL)
	return -1;

    if (ii->ii_dbc == NULL)
	ii->ii_dbc = dbiCursorInit(ii->ii_dbi, DBC_READ);

    /* free old data */
    ii->ii_set = dbiIndexSetFree(ii->ii_set);

    rc = idxdbGet(ii->ii_dbi, ii->ii_dbc, NULL, 0, &ii->ii_set, DBC_NORMAL_SEARCH);

    *key = idxdbKey(ii->ii_dbi, ii->ii_dbc, &iikeylen);
    *keylen = iikeylen;

    return (rc == RPMRC_OK) ? 0 : -1;
}

int rpmdbIndexIteratorNextTd(rpmdbIndexIterator ii, rpmtd keytd)
{
    size_t keylen = 0;
    const void * keyp = NULL;

    int rc = rpmdbIndexIteratorNext(ii, &keyp, &keylen);

    if (rc == 0) {
	rpmTagVal tag = ii->ii_rpmtag;
	rpmTagClass tagclass = rpmTagGetClass(tag);

	/* Set the common values, overridden below as necessary */
	keytd->type = rpmTagGetTagType(tag);
	keytd->tag = tag;
	keytd->flags = RPMTD_ALLOCED;
	keytd->count = 1;

	switch (tagclass) {
	case RPM_STRING_CLASS: {
	    /*
	     * XXX: We never return arrays here, so everything is a
	     * "simple" string. However this can disagree with the
	     * type of the index tag, eg requires are string arrays.
	     */
	    char *key = memcpy(xmalloc(keylen + 1), keyp, keylen);
	    key[keylen] = '\0';
	    keytd->data = key;
	    keytd->type = RPM_STRING_TYPE;
	    } break;
	case RPM_BINARY_CLASS:
	    /* Binary types abuse count for data length */
	    keytd->count = keylen;
	    /* fallthrough */
	case RPM_NUMERIC_CLASS:
	    keytd->data = memcpy(xmalloc(keylen), keyp, keylen);
	    break;
	default:
	    rpmtdReset(keytd);
	    rc = -1;
	    break;
	}
    }
    
    return rc;
}

unsigned int rpmdbIndexIteratorNumPkgs(rpmdbIndexIterator ii)
{
    return (ii && ii->ii_set) ? dbiIndexSetCount(ii->ii_set) : 0;
}

unsigned int rpmdbIndexIteratorPkgOffset(rpmdbIndexIterator ii, unsigned int nr)
{
    if (!ii || !ii->ii_set)
        return 0;
    if (dbiIndexSetCount(ii->ii_set) <= nr)
        return 0;
    return dbiIndexRecordOffset(ii->ii_set, nr);
}

unsigned int *rpmdbIndexIteratorPkgOffsets(rpmdbIndexIterator ii)
{
    int i;

    if (!ii || !ii->ii_set)
	return NULL;

    if (ii->ii_hdrNums)
	ii->ii_hdrNums = _free(ii->ii_hdrNums);

    ii->ii_hdrNums = xmalloc(sizeof(*ii->ii_hdrNums) * ii->ii_set->count);
    for (i = 0; i < ii->ii_set->count; i++) {
	ii->ii_hdrNums[i] = ii->ii_set->recs[i].hdrNum;
    }

    return ii->ii_hdrNums;
}

unsigned int rpmdbIndexIteratorTagNum(rpmdbIndexIterator ii, unsigned int nr)
{
    if (!ii || !ii->ii_set)
        return 0;
    if (dbiIndexSetCount(ii->ii_set) <= nr)
        return 0;
    return dbiIndexRecordFileNumber(ii->ii_set, nr);
}

rpmdbIndexIterator rpmdbIndexIteratorFree(rpmdbIndexIterator ii)
{
    rpmdbIndexIterator * prev, next;

    if (ii == NULL)
        return NULL;

    prev = &rpmiiRock;
    while ((next = *prev) != NULL && next != ii)
        prev = &next->ii_next;
    if (next) {
        *prev = next->ii_next;
        next->ii_next = NULL;
    } else
	return NULL;

    ii->ii_dbc = dbiCursorFree(ii->ii_dbi, ii->ii_dbc);
    ii->ii_dbi = NULL;
    rpmdbClose(ii->ii_db);
    ii->ii_set = dbiIndexSetFree(ii->ii_set);

    if (ii->ii_hdrNums)
	ii->ii_hdrNums = _free(ii->ii_hdrNums);

    ii = _free(ii);
    return NULL;
}

static void logAddRemove(const char *dbiname, int removing, rpmtd tagdata)
{
    rpm_count_t c = rpmtdCount(tagdata);
    if (c == 1 && rpmtdType(tagdata) == RPM_STRING_TYPE) {
	rpmlog(RPMLOG_DEBUG, "%s \"%s\" %s %s index.\n",
		removing ? "removing" : "adding", rpmtdGetString(tagdata), 
		removing ? "from" : "to", dbiname);
    } else if (c > 0) {
	rpmlog(RPMLOG_DEBUG, "%s %d entries %s %s index.\n",
		removing ? "removing" : "adding", c, 
		removing ? "from" : "to", dbiname);
    }
}

static rpmRC indexDel(dbiIndex dbi, rpmTagVal rpmtag, unsigned int hdrNum, Header h)
{
    return tag2index(dbi, rpmtag, hdrNum, h, idxdbDel);
}

int rpmdbRemove(rpmdb db, unsigned int hdrNum)
{
    dbiIndex dbi = NULL;
    dbiCursor dbc = NULL;
    Header h;
    sigset_t signalMask;
    int ret = 0;

    if (db == NULL)
	return 0;

    h = rpmdbGetHeaderAt(db, hdrNum);

    if (h == NULL) {
	rpmlog(RPMLOG_ERR, _("%s: cannot read header at 0x%x\n"),
	      "rpmdbRemove", hdrNum);
	return 1;
    } else {
	char *nevra = headerGetAsString(h, RPMTAG_NEVRA);
	rpmlog(RPMLOG_DEBUG, "  --- h#%8u %s\n", hdrNum, nevra);
	free(nevra);
    }

    if (pkgdbOpen(db, 0, &dbi))
	return 1;

    (void) blockSignals(&signalMask);
    dbCtrl(db, DB_CTRL_LOCK_RW);

    /* Remove header from primary index */
    dbc = dbiCursorInit(dbi, DBC_WRITE);
    ret = pkgdbDel(dbi, dbc, hdrNum);
    dbiCursorFree(dbi, dbc);

    /* Remove associated data from secondary indexes */
    if (ret == 0) {
	for (int dbix = 0; dbix < db->db_ndbi; dbix++) {
	    rpmDbiTag rpmtag = db->db_tags[dbix];

	    if (indexOpen(db, rpmtag, 0, &dbi))
		continue;

	    ret += indexDel(dbi, rpmtag, hdrNum, h);
	}
    }

    dbCtrl(db, DB_CTRL_INDEXSYNC);
    dbCtrl(db, DB_CTRL_UNLOCK_RW);
    (void) unblockSignals(&signalMask);

    headerFree(h);

    /* XXX return ret; */
    return 0;
}

struct updateRichDepData {
    ARGV_t argv;
    int nargv;
    int neg;
    int level;
    int *nargv_level;
};

static rpmRC updateRichDepCB(void *cbdata, rpmrichParseType type,
		const char *n, int nl, const char *e, int el, rpmsenseFlags sense,
		rpmrichOp op, char **emsg) {
    struct updateRichDepData *data = cbdata;
    if (type == RPMRICH_PARSE_ENTER) {
	data->level++;
	data->nargv_level = xrealloc(data->nargv_level, data->level * (sizeof(int)));
	data->nargv_level[data->level - 1] = data->nargv;
    }
    if (type == RPMRICH_PARSE_LEAVE) {
	data->level--;
    }
    if (type == RPMRICH_PARSE_SIMPLE && nl && !(nl > 7 && !strncmp(n, "rpmlib(", 7))) {
	char *name = xmalloc(nl + 2);
	*name = data->neg ? '!' : ' ';
	strncpy(name + 1, n, nl);
	name[1 + nl] = 0;
	argvAdd(&data->argv, name);
	data->nargv++;
	_free(name);
    }
    if (type == RPMRICH_PARSE_OP && op == RPMRICHOP_IF) {
	/* save nargv in case of ELSE */
	data->nargv_level[data->level - 1] = data->nargv;
	data->neg ^= 1;
    }
    if (type == RPMRICH_PARSE_OP && op == RPMRICHOP_ELSE) {
	int i, nargv = data->nargv;
	/* copy and invert condition block */
	for (i = data->nargv_level[data->level - 1]; i < nargv; i++) {
	    char *name = data->argv[i];
	    *name ^= ' ' ^ '!';
	    argvAdd(&data->argv, name);
	    *name ^= ' ' ^ '!';
	    data->nargv++;
	}
	data->neg ^= 1;
    }
    if (type == RPMRICH_PARSE_LEAVE && op == RPMRICHOP_IF) {
	data->neg ^= 1;
    }
    return RPMRC_OK;
}

static rpmRC updateRichDep(dbiIndex dbi, dbiCursor dbc, const char *str,
                           struct dbiIndexItem_s *rec,
                           idxfunc idxupdate)
{
    int n, i, rc = 0;
    struct updateRichDepData data;

    data.argv = argvNew();
    data.neg = 0;
    data.nargv = 0;
    data.level = 0;
    data.nargv_level = xcalloc(1, sizeof(int));
    if (rpmrichParse(&str, NULL, updateRichDepCB, &data) == RPMRC_OK) {
	n = argvCount(data.argv);
	if (n) {
	    argvSort(data.argv, NULL);
	    for (i = 0; i < n; i++) {
		char *name = data.argv[i];
		if (i && !strcmp(data.argv[i - 1], name))
		    continue;       /* ignore dups */
		if (*name == ' ')
		    name++;
		rc += idxupdate(dbi, dbc, name, strlen(name), rec);
	    }
	}
    }
    _free(data.nargv_level);
    argvFree(data.argv);
    return rc;
}

static rpmRC tag2index(dbiIndex dbi, rpmTagVal rpmtag,
		       unsigned int hdrNum, Header h,
		       idxfunc idxupdate)
{
    int i, rc = 0;
    struct rpmtd_s tagdata, reqflags, trig_index;
    dbiCursor dbc = NULL;

    switch (rpmtag) {
    case RPMTAG_REQUIRENAME:
	headerGet(h, RPMTAG_REQUIREFLAGS, &reqflags, HEADERGET_MINMEM);
	break;
    case RPMTAG_FILETRIGGERNAME:
	headerGet(h, RPMTAG_FILETRIGGERINDEX, &trig_index, HEADERGET_MINMEM);
	break;
    case RPMTAG_TRANSFILETRIGGERNAME:
	headerGet(h, RPMTAG_TRANSFILETRIGGERINDEX, &trig_index, HEADERGET_MINMEM);
	break;
    }
    headerGet(h, rpmtag, &tagdata, HEADERGET_MINMEM);

    if (rpmtdCount(&tagdata) == 0) {
	if (rpmtag != RPMTAG_GROUP)
	    goto exit;

	/* XXX preserve legacy behavior */
	tagdata.type = RPM_STRING_TYPE;
	tagdata.data = (const char **) "Unknown";
	tagdata.count = 1;
    }

    dbc = dbiCursorInit(dbi, DBC_WRITE);

    logAddRemove(dbiName(dbi), 0, &tagdata);
    while ((i = rpmtdNext(&tagdata)) >= 0) {
	const void * key = NULL;
	unsigned int keylen = 0;
	int j;
	struct dbiIndexItem_s rec;

	switch (rpmtag) {
	/* Include trigger index in db index for triggers */
	case RPMTAG_FILETRIGGERNAME:
	case RPMTAG_TRANSFILETRIGGERNAME:
	    rec.hdrNum = hdrNum;
	    rec.tagNum = *rpmtdNextUint32(&trig_index);
	    break;

	/* Include the tagNum in the others indices (only files use though) */
	default:
	    rec.hdrNum = hdrNum;
	    rec.tagNum = i;
	    break;
	}

	switch (rpmtag) {
	case RPMTAG_REQUIRENAME: {
	    /* Filter out install prerequisites. */
	    rpm_flag_t *rflag = rpmtdNextUint32(&reqflags);
	    if (rflag && isInstallPreReq(*rflag) &&
			 !isErasePreReq(*rflag))
		continue;
	    break;
	    }
	case RPMTAG_TRIGGERNAME:
	    if (i > 0) {	/* don't add duplicates */
		const char **tnames = tagdata.data;
		const char *str = rpmtdGetString(&tagdata);
		for (j = 0; j < i; j++) {
		    if (rstreq(str, tnames[j]))
			break;
		}
		if (j < i)
		    continue;
	    }
	    break;
	default:
	    break;
	}

	if ((key = td2key(&tagdata, &keylen)) == NULL)
	    continue;

	rc += idxupdate(dbi, dbc, key, keylen, &rec);

	if (*(char *)key == '(') {
	    switch (rpmtag) {
	    case RPMTAG_REQUIRENAME:
	    case RPMTAG_CONFLICTNAME:
	    case RPMTAG_SUGGESTNAME:
	    case RPMTAG_SUPPLEMENTNAME:
	    case RPMTAG_RECOMMENDNAME:
	    case RPMTAG_ENHANCENAME:
		if (rpmtdType(&tagdata) == RPM_STRING_ARRAY_TYPE) {
		    rc += updateRichDep(dbi, dbc, rpmtdGetString(&tagdata),
			&rec, idxupdate);
		}
	    default:
		break;
	    }
	}
    }

    dbiCursorFree(dbi, dbc);

exit:
    rpmtdFreeData(&tagdata);
    return (rc == 0) ? RPMRC_OK : RPMRC_FAIL;
}

static rpmRC indexPut(dbiIndex dbi, rpmTagVal rpmtag, unsigned int hdrNum, Header h)
{
    return tag2index(dbi, rpmtag, hdrNum, h, idxdbPut);
}

int rpmdbAdd(rpmdb db, Header h)
{
    sigset_t signalMask;
    dbiIndex dbi = NULL;
    dbiCursor dbc = NULL;
    unsigned int hdrNum = 0;
    unsigned int hdrLen = 0;
    unsigned char *hdrBlob = NULL;
    int ret = 0;

    if (db == NULL)
	return 0;

    hdrBlob = headerExport(h, &hdrLen);
    if (hdrBlob == NULL || hdrLen == 0) {
	ret = -1;
	goto exit;
    }

    ret = pkgdbOpen(db, 0, &dbi);
    if (ret)
	goto exit;
	
    (void) blockSignals(&signalMask);
    dbCtrl(db, DB_CTRL_LOCK_RW);

    /* Add header to primary index */
    dbc = dbiCursorInit(dbi, DBC_WRITE);
    ret = pkgdbNew(dbi, dbc, &hdrNum);
    if (ret == 0)
	ret = pkgdbPut(dbi, dbc, hdrNum, hdrBlob, hdrLen);
    dbiCursorFree(dbi, dbc);

    /* Add associated data to secondary indexes */
    if (ret == 0) {	
	for (int dbix = 0; dbix < db->db_ndbi; dbix++) {
	    rpmDbiTag rpmtag = db->db_tags[dbix];

	    if (indexOpen(db, rpmtag, 0, &dbi))
		continue;

	    ret += indexPut(dbi, rpmtag, hdrNum, h);
	}
    }

    dbCtrl(db, DB_CTRL_INDEXSYNC);
    dbCtrl(db, DB_CTRL_UNLOCK_RW);
    (void) unblockSignals(&signalMask);

    /* If everything ok, mark header as installed now */
    if (ret == 0) {
	headerSetInstance(h, hdrNum);
	/* Purge our verification cache on added public keys */
	if (db->db_checked && headerIsEntry(h, RPMTAG_PUBKEYS)) {
	    dbChkEmpty(db->db_checked);
	}
    }

exit:
    free(hdrBlob);

    return ret;
}

/*
 * Remove DB4 environment (and lock), ie the equivalent of 
 * rm -f <prefix>/<dbpath>/__db.???
 * Environment files not existing is not an error, failure to unlink is,
 * return zero on success.
 * TODO/FIX: push this down to db3.c where it belongs
 */
static int cleanDbenv(const char *prefix, const char *dbpath)
{
    ARGV_t paths = NULL, p;
    int rc = 0; 
    char *pattern = rpmGetPath(prefix, "/", dbpath, "/__db.???", NULL);

    if (rpmGlob(pattern, NULL, &paths) == 0) {
	for (p = paths; *p; p++) {
	    rc += unlink(*p);
	}
	argvFree(paths);
    }
    free(pattern);
    return rc;
}

static int unlinkTag(const char * prefix, const char *dbpath, rpmTagVal dbtag)
{
    int rc = 0;
    const char * base = rpmTagGetName(dbtag);
    char * path = rpmGetPath(prefix, "/", dbpath, "/", base, NULL);
    if (access(path, F_OK) == 0)
	rc = unlink(path);
    free(path);
    return rc;
}

static int rpmdbRemoveDatabase(const char * prefix, const char * dbpath)
{ 
    char *path;
    int xx = 0;
    /* create a handle but dont actually open */
    rpmdb db = newRpmdb(prefix, dbpath, O_RDONLY, 0644, RPMDB_FLAG_REBUILD);

    xx = unlinkTag(prefix, dbpath, RPMDBI_PACKAGES);
    for (int i = 0; i < db->db_ndbi; i++) {
	xx += unlinkTag(prefix, dbpath, db->db_tags[i]);
    }
    cleanDbenv(prefix, dbpath);

    path = rpmGetPath(prefix, "/", dbpath, NULL);
    xx += rmdir(path);
    free(path);
    rpmdbClose(db);

    return (xx != 0);
}

static int renameTag(const char * prefix,
		     const char * olddbpath, const char *newdbpath,
		     const char * base)
{
    int xx, rc = 0;
    char *src = rpmGetPath(prefix, "/", olddbpath, "/", base, NULL);
    char *dest = rpmGetPath(prefix, "/", newdbpath, "/", base, NULL);
    struct stat st;

    if (access(src, F_OK) == 0) {
	/*
	 * Restore uid/gid/mode/security context if possible.
	 */
	if (stat(dest, &st) < 0)
	    if (stat(src, &st) < 0)
		goto exit;

	if ((xx = rename(src, dest)) != 0) {
	    rc = 1;
	    goto exit;
	}
	xx = chown(dest, st.st_uid, st.st_gid);
	xx = chmod(dest, (st.st_mode & 07777));

	/* XXX: we should call file prepare plugins here for selinux etc! */
    }

exit:
    free(src);
    free(dest);
    return rc;
}

static int rpmdbMoveDatabase(const char * prefix,
			     const char * olddbpath, const char * newdbpath)
{
    int rc = 0;
    sigset_t sigMask;
    /* create a handle but dont actually open */
    rpmdb db = newRpmdb(prefix, newdbpath, O_RDONLY, 0644, RPMDB_FLAG_REBUILD);

    blockSignals(&sigMask);
    rc = renameTag(prefix, olddbpath, newdbpath, rpmTagGetName(RPMDBI_PACKAGES));
    for (int i = 0; i < db->db_ndbi; i++) {
	rc += renameTag(prefix, olddbpath, newdbpath, rpmTagGetName(db->db_tags[i]));
    }
#ifdef ENABLE_NDB
    rc += renameTag(prefix, olddbpath, newdbpath, "Packages.db");
    rc += renameTag(prefix, olddbpath, newdbpath, "Index.db");
#endif

    cleanDbenv(prefix, olddbpath);
    cleanDbenv(prefix, newdbpath);
    rpmdbClose(db);

    unblockSignals(&sigMask);

    return rc;
}

int rpmdbRebuild(const char * prefix, rpmts ts,
		rpmRC (*hdrchk) (rpmts ts, const void *uh, size_t uc, char ** msg))
{
    rpmdb olddb;
    char * dbpath = NULL;
    char * rootdbpath = NULL;
    rpmdb newdb;
    char * newdbpath = NULL;
    char * newrootdbpath = NULL;
    int nocleanup = 1;
    int failed = 0;
    int removedir = 0;
    int rc = 0;

    dbpath = rpmGetPath("%{?_dbpath}", NULL);
    if (rstreq(dbpath, "")) {
	rpmlog(RPMLOG_ERR, _("no dbpath has been set"));
	rc = 1;
	goto exit;
    }
    rootdbpath = rpmGetPath(prefix, dbpath, NULL);

    newdbpath = rpmGetPath("%{?_dbpath_rebuild}", NULL);
    if (rstreq(newdbpath, "") || rstreq(newdbpath, dbpath)) {
	newdbpath = _free(newdbpath);
	rasprintf(&newdbpath, "%srebuilddb.%d", dbpath, (int) getpid());
	nocleanup = 0;
    }
    newrootdbpath = rpmGetPath(prefix, newdbpath, NULL);

    rpmlog(RPMLOG_DEBUG, "rebuilding database %s into %s\n",
	rootdbpath, newrootdbpath);

    if (mkdir(newrootdbpath, 0755)) {
	rpmlog(RPMLOG_ERR, _("failed to create directory %s: %s\n"),
	      newrootdbpath, strerror(errno));
	rc = 1;
	goto exit;
    }
    removedir = 1;

    if (openDatabase(prefix, dbpath, &olddb,
		     O_RDONLY, 0644, RPMDB_FLAG_REBUILD)) {
	rc = 1;
	goto exit;
    }
    if (openDatabase(prefix, newdbpath, &newdb,
		     (O_RDWR | O_CREAT), 0644, RPMDB_FLAG_REBUILD)) {
	rc = 1;
	goto exit;
    }

    {	Header h = NULL;
	rpmdbMatchIterator mi;

	mi = rpmdbInitIterator(olddb, RPMDBI_PACKAGES, NULL, 0);
	if (ts && hdrchk)
	    (void) rpmdbSetHdrChk(mi, ts, hdrchk);

	while ((h = rpmdbNextIterator(mi)) != NULL) {

	    /* let's sanity check this record a bit, otherwise just skip it */
	    if (!(headerIsEntry(h, RPMTAG_NAME) &&
		headerIsEntry(h, RPMTAG_VERSION) &&
		headerIsEntry(h, RPMTAG_RELEASE) &&
		headerIsEntry(h, RPMTAG_BUILDTIME)))
	    {
		rpmlog(RPMLOG_ERR,
			_("header #%u in the database is bad -- skipping.\n"),
			rpmdbGetIteratorOffset(mi));
		continue;
	    }

	    /* Deleted entries are eliminated in legacy headers by copy. */
	    {	Header nh = (headerIsEntry(h, RPMTAG_HEADERIMAGE)
				? headerCopy(h) : NULL);
		rc = rpmdbAdd(newdb, (nh ? nh : h));
		headerFree(nh);
	    }

	    if (rc) {
		rpmlog(RPMLOG_ERR, _("cannot add record originally at %u\n"),
		       rpmdbGetIteratorOffset(mi));
		failed = 1;
		break;
	    }
	}

	rpmdbFreeIterator(mi);

    }

    rpmdbClose(olddb);
    rpmdbClose(newdb);

    if (failed) {
	rpmlog(RPMLOG_WARNING, 
		_("failed to rebuild database: original database "
		"remains in place\n"));

	rpmdbRemoveDatabase(prefix, newdbpath);
	rc = 1;
	goto exit;
    } else if (!nocleanup) {
	if (rpmdbMoveDatabase(prefix, newdbpath, dbpath)) {
	    rpmlog(RPMLOG_ERR, _("failed to replace old database with new "
			"database!\n"));
	    rpmlog(RPMLOG_ERR, _("replace files in %s with files from %s "
			"to recover"), dbpath, newdbpath);
	    rc = 1;
	    goto exit;
	}
    }
    rc = 0;

exit:
    if (removedir && !(rc == 0 && nocleanup)) {
	if (rmdir(newrootdbpath))
	    rpmlog(RPMLOG_ERR, _("failed to remove directory %s: %s\n"),
			newrootdbpath, strerror(errno));
    }
    free(newdbpath);
    free(dbpath);
    free(newrootdbpath);
    free(rootdbpath);

    return rc;
}

int rpmdbCtrl(rpmdb db, rpmdbCtrlOp ctrl)
{
    dbCtrlOp dbctrl = 0;
    switch (ctrl) {
    case RPMDB_CTRL_LOCK_RO:
	dbctrl = DB_CTRL_LOCK_RO;
	break;
    case RPMDB_CTRL_UNLOCK_RO:
	dbctrl = DB_CTRL_UNLOCK_RO;
	break;
    case RPMDB_CTRL_LOCK_RW:
	dbctrl = DB_CTRL_LOCK_RW;
	break;
    case RPMDB_CTRL_UNLOCK_RW:
	dbctrl = DB_CTRL_UNLOCK_RW;
	break;
    case RPMDB_CTRL_INDEXSYNC:
	dbctrl = DB_CTRL_INDEXSYNC;
	break;
    }
    return dbctrl ? dbCtrl(db, dbctrl) : 1;
}

