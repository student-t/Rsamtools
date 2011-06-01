#include "tabixfile.h"
#include "utilities.h"

static SEXP TABIXFILE_TAG = NULL;

static void
_tabixfile_close(SEXP ext)
{
    _TABIX_FILE *tfile = TABIXFILE(ext);
    if (NULL != tfile->tabix)
	ti_close(tfile->tabix);
    tfile->tabix = NULL;
    if (NULL != tfile->iter)
	ti_iter_destroy(tfile->iter);
    tfile->iter = NULL;
}

static void
_tabixfile_finalizer(SEXP ext)
{
    if (NULL == R_ExternalPtrAddr(ext))
	return;
    _tabixfile_close(ext);
    _TABIX_FILE *tfile = TABIXFILE(ext);
    Free(tfile);
    R_SetExternalPtrAddr(ext, NULL);
}

SEXP
tabixfile_init()
{
    TABIXFILE_TAG = install("TabixFile");
    return R_NilValue;
}

SEXP
tabixfile_open(SEXP filename, SEXP indexname)
{
    if (!IS_CHARACTER(filename) || 1L != Rf_length(filename))
	Rf_error("'filename' must be character(1)");
    if (!IS_CHARACTER(indexname) || 1L != Rf_length(indexname))
	Rf_error("'indexname' must be character(1)");

    _TABIX_FILE *tfile = Calloc(1, _TABIX_FILE);
    tfile->tabix = ti_open(translateChar(STRING_ELT(filename, 0)),
			   translateChar(STRING_ELT(indexname, 0)));
    if (NULL == tfile->tabix) {
	Free(tfile);
	Rf_error("failed to open file");
    }
    tfile->iter = NULL;

    SEXP ext =
	PROTECT(R_MakeExternalPtr(tfile, TABIXFILE_TAG, filename));
    R_RegisterCFinalizerEx(ext, _tabixfile_finalizer, TRUE);
    UNPROTECT(1);

    return ext;
}

SEXP
tabixfile_close(SEXP ext)
{
    _scan_checkext(ext, TABIXFILE_TAG, "close");
    _tabixfile_close(ext);
    return(ext);
}

SEXP
tabixfile_isopen(SEXP ext)
{
    SEXP ans = ScalarLogical(FALSE);
    if (NULL != TABIXFILE(ext)) {
	_scan_checkext(ext, TABIXFILE_TAG, "isOpen");
	if (TABIXFILE(ext)->tabix)
	    ans = ScalarLogical(TRUE);
    }
    return ans;
}

SEXP
index_tabix(SEXP filename, SEXP format,
	    SEXP seq, SEXP begin, SEXP end,
	    SEXP skip, SEXP comment, SEXP zeroBased)
{
    ti_conf_t conf = ti_conf_gff;

    if (!IS_CHARACTER(filename) || 1L != Rf_length(filename))
	Rf_error("'filename' must be character(1)");
    if (1L == Rf_length(format)) {
	const char *txt = CHAR(STRING_ELT(format, 0));
	if (strcmp(txt, "gff") == 0) conf = ti_conf_gff;
	else if (strcmp(txt, "bed") == 0) conf = ti_conf_bed;
	else if (strcmp(txt, "sam") == 0) conf = ti_conf_sam;
	else if (strcmp(txt, "vcf") == 0 ||
		 strcmp(txt, "vcf4") == 0) conf = ti_conf_vcf;
	else if (strcmp(txt, "psltbl") == 0) conf = ti_conf_psltbl;
	else
	    Rf_error("format '%s' unrecognized", txt);
    } else {
	if (!IS_INTEGER(seq) || 1L != Rf_length(seq))
	    Rf_error("'seq' must be integer(1)");
	conf.sc = INTEGER(seq)[0];
	if (!IS_INTEGER(begin) || 1L != Rf_length(begin))
	    Rf_error("'begin' must be integer(1)");
	conf.bc = INTEGER(begin)[0];
	if (!IS_INTEGER(end) || 1L != Rf_length(end))
	    Rf_error("'end' must be integer(1)");
	conf.ec = INTEGER(end)[0];
    }

    if (IS_INTEGER(skip) && 1L == Rf_length(skip))
	conf.line_skip = INTEGER(skip)[0];
    if  (IS_CHARACTER(comment) && 1L == Rf_length(comment))
	conf.meta_char = CHAR(STRING_ELT(comment, 0))[0];
    if (IS_LOGICAL(zeroBased) && 1L == Rf_length(zeroBased))
	conf.preset |= TI_FLAG_UCSC;

    int res = ti_index_build(translateChar(STRING_ELT(filename, 0)),
			     &conf);

    if (-1L == res)
	Rf_error("index build failed");

    return filename;
}

SEXP
_header_lines(tabix_t *tabix, const ti_conf_t *conf)
{
    const int GROW_BY = 100;
    SEXP lns;
    int i_lns = 0, pidx;

    ti_iter_t iter = ti_query(tabix, NULL, 0, 0);
    const char *s;
    int len;

    if (NULL == iter)
	Rf_error("failed to obtain tabix iterator");

    PROTECT_WITH_INDEX(lns = NEW_CHARACTER(0), &pidx);
    while (NULL != (s = ti_read(tabix, iter, &len))) {
	if ((int)(*s) != conf->meta_char) break;
	if (0 == (i_lns % GROW_BY)) {
	    lns = Rf_lengthgets(lns, Rf_length(lns) + GROW_BY);
	    REPROTECT(lns, pidx);
	}
	SET_STRING_ELT(lns, i_lns++, mkChar(s));
    }
    ti_iter_destroy(iter);

    lns = Rf_lengthgets(lns, i_lns);
    UNPROTECT(1);

    return lns;
}

SEXP
header_tabix(SEXP ext)
{
    _scan_checkext(ext, TABIXFILE_TAG, "scanTabix");
    tabix_t *tabix = TABIXFILE(ext)->tabix;
    if (0 != ti_lazy_index_load(tabix))
	Rf_error("'seqnamesTabix' failed to load index");

    SEXP result = PROTECT(NEW_LIST(5)), tmp, nms;
    nms = NEW_CHARACTER(Rf_length(result));
    Rf_namesgets(result, nms);
    SET_STRING_ELT(nms, 0, mkChar("seqnames"));
    SET_STRING_ELT(nms, 1, mkChar("indexColumns"));
    SET_STRING_ELT(nms, 2, mkChar("skip"));
    SET_STRING_ELT(nms, 3, mkChar("comment"));
    SET_STRING_ELT(nms, 4, mkChar("header"));

    /* seqnames */
    int n;
    const char **seqnames = ti_seqname(tabix->idx, &n);
    if (n < 0)
	Rf_error("'seqnamesTabix' found <0 (!) seqnames");
    tmp = NEW_CHARACTER(n);
    SET_VECTOR_ELT(result, 0, tmp);
    for (int i = 0; i < n; ++i)
	SET_STRING_ELT(tmp, i, mkChar(seqnames[i]));
    free(seqnames);

    const ti_conf_t *conf = ti_get_conf(tabix->idx);

    /* indexColumns */
    tmp = NEW_INTEGER(3);
    SET_VECTOR_ELT(result, 1, tmp);
    INTEGER(tmp)[0] = conf->sc;
    INTEGER(tmp)[1] = conf->bc;
    INTEGER(tmp)[2] = conf->ec;
    nms = NEW_CHARACTER(3);
    Rf_namesgets(tmp, nms);
    SET_STRING_ELT(nms, 0, mkChar("seq"));
    SET_STRING_ELT(nms, 1, mkChar("start"));
    SET_STRING_ELT(nms, 2, mkChar("end"));

    /* skip */
    SET_VECTOR_ELT(result, 2, ScalarInteger(conf->line_skip));

    /* comment */
    char comment[2];
    comment[0] = (char) conf->meta_char;
    comment[1] = '\0';
    SET_VECTOR_ELT(result, 3, ScalarString(mkChar(comment)));

    /* header lines */
    SET_VECTOR_ELT(result, 4, _header_lines(tabix, conf));

    UNPROTECT(1);
    return result;
}

SEXP
scan_tabix(SEXP ext, SEXP space, SEXP yieldSize)
{
    const double REC_SCALE = 1.4; /* scaling factor when pre-allocated
				   * result needs to grow */
    _scan_checkparams(space, R_NilValue, R_NilValue);
    if (!IS_INTEGER(yieldSize) || 1L != Rf_length(yieldSize))
	Rf_error("'yieldSize' must be integer(1)");
    _scan_checkext(ext, TABIXFILE_TAG, "scanTabix");

    tabix_t *tabix = TABIXFILE(ext)->tabix;
    if (0 != ti_lazy_index_load(tabix))
	Rf_error("'scanTabix' failed to load index");

    SEXP spc = VECTOR_ELT(space, 0);
    const int
	*start = INTEGER(VECTOR_ELT(space, 1)),
	*end = INTEGER(VECTOR_ELT(space, 2)),
	nspc = Rf_length(spc);


    SEXP result = PROTECT(NEW_LIST(nspc));

    int buflen = 4096;
    char *buf = Calloc(buflen, char);

    for (int ispc = 0; ispc < nspc; ++ispc) {
	int totrec = INTEGER(yieldSize)[0];
	SEXP records = NEW_CHARACTER(totrec);
	SET_VECTOR_ELT(result, ispc, records); /* protect */

	int tid;
	const char *s = CHAR(STRING_ELT(spc, ispc));
	if (0 > (tid = ti_get_tid(tabix->idx, s)))
	    Rf_error("'%s' not present in tabix index", s);
	ti_iter_t iter =
	    ti_iter_query(tabix->idx, tid, start[ispc], end[ispc]);

	int linelen;
	const char *line;
	int irec = 0;
	while (NULL != (line = ti_read(tabix, iter, &linelen))) {
	    if (totrec < irec) { /* grow */
		totrec *= REC_SCALE;
		records = Rf_lengthgets(records, totrec);
		SET_VECTOR_ELT(result, ispc, records);
	    }
	    if (linelen + 1 > buflen) {
		Free(buf);
		buflen = 2 * linelen;
		buf = Calloc(buflen, char);
	    }
	    memcpy(buf, line, linelen);
	    buf[linelen] = '\0';
	    SET_STRING_ELT(records, irec, mkChar(buf));
	    irec += 1;
	}

	ti_iter_destroy(iter);
	records = Rf_lengthgets(records, irec);
	SET_VECTOR_ELT(result, ispc, records);
    }

    Free(buf);
    UNPROTECT(1);
    return result;
}

SEXP
yield_tabix(SEXP ext, SEXP yieldSize)
{
    if (!IS_INTEGER(yieldSize) || 1L != Rf_length(yieldSize))
	Rf_error("'yieldSize' must be integer(1)");
    _scan_checkext(ext, TABIXFILE_TAG, "scanTabix");

    tabix_t *tabix = TABIXFILE(ext)->tabix;
    ti_iter_t iter = TABIXFILE(ext)->iter;

    if (NULL == iter) {
	if (0 != ti_lazy_index_load(tabix))
	    Rf_error("'scanTabix' failed to load index");
	iter = TABIXFILE(ext)->iter = ti_iter_first();
    }

    int buflen = 4096;
    char *buf = Calloc(buflen, char);
    int linelen;
    const char *line;

    int totrec = INTEGER(yieldSize)[0];
    SEXP result = PROTECT(NEW_CHARACTER(totrec));

    int irec = 0;
    while (irec < totrec) {
	line = ti_read(tabix, iter, &linelen);
	if (NULL == line) break;
	if (linelen + 1 > buflen) {
	    Free(buf);
	    buflen = 2 * linelen;
	    buf = Calloc(buflen, char);
	}
	memcpy(buf, line, linelen);
	buf[linelen] = '\0';
	SET_STRING_ELT(result, irec, mkChar(buf));
	irec += 1;
    }

    Free(buf);
    if (irec != totrec)
	result = Rf_lengthgets(result, irec);
    UNPROTECT(1);
    return result;
}