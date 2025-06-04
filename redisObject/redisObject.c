#include "redisObject.h"
/* ------------------------ Lexicographic ranges ---------------------------- */

/* Parse max or min argument of ZRANGEBYLEX.
  * (foo means foo (open interval)
  * [foo means foo (closed interval)
  * - means the min string possible
  * + means the max string possible
  *
  * If the string is valid the *dest pointer is set to the redis object
  * that will be used for the comparison, and ex will be set to 0 or 1
  * respectively if the item is exclusive or inclusive. C_OK will be
  * returned.
  *
  * If the string is not a valid range C_ERR is returned, and the value
  * of *dest and *ex is undefined. */
int zslParseLexRangeItem(robj *item, sds *dest, int *ex) {
    char *c = item->ptr;

    switch(c[0]) {
    case '+':
        if (c[1] != '\0') return C_ERR;
        *ex = 1;
        *dest = shared.maxstring;
        return C_OK;
    case '-':
        if (c[1] != '\0') return C_ERR;
        *ex = 1;
        *dest = shared.minstring;
        return C_OK;
    case '(':
        *ex = 1;
        *dest = sdsnewlen(c+1,sdslen(c)-1);
        return C_OK;
    case '[':
        *ex = 0;
        *dest = sdsnewlen(c+1,sdslen(c)-1);
        return C_OK;
    default:
        return C_ERR;
    }
}

/* Free a lex range structure, must be called only after zelParseLexRange()
 * populated the structure with success (C_OK returned). */
void zslFreeLexRange(zlexrangespec *spec) {
    if (spec->min != shared.minstring &&
        spec->min != shared.maxstring) sdsfree(spec->min);
    if (spec->max != shared.minstring &&
        spec->max != shared.maxstring) sdsfree(spec->max);
}

/* Populate the lex rangespec according to the objects min and max.
 *
 * Return C_OK on success. On error C_ERR is returned.
 * When OK is returned the structure must be freed with zslFreeLexRange(),
 * otherwise no release is needed. */
int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec) {
    /* The range can't be valid if objects are integer encoded.
     * Every item must start with ( or [. */
    if (min->encoding == OBJ_ENCODING_INT ||
        max->encoding == OBJ_ENCODING_INT) return C_ERR;

    spec->min = spec->max = NULL;
    if (zslParseLexRangeItem(min, &spec->min, &spec->minex) == C_ERR ||
        zslParseLexRangeItem(max, &spec->max, &spec->maxex) == C_ERR) {
        zslFreeLexRange(spec);
        return C_ERR;
    } else {
        return C_OK;
    }
}

/* This is just a wrapper to sdscmp() that is able to
 * handle shared.minstring and shared.maxstring as the equivalent of
 * -inf and +inf for strings */
int sdscmplex(sds a, sds b) {
    if (a == b) return 0;
    if (a == shared.minstring || b == shared.maxstring) return -1;
    if (a == shared.maxstring || b == shared.minstring) return 1;
    return sdscmp(a,b);
}

int zslLexValueGteMin(sds value, zlexrangespec *spec) {
    return spec->minex ?
        (sdscmplex(value,spec->min) > 0) :
        (sdscmplex(value,spec->min) >= 0);
}

int zslLexValueLteMax(sds value, zlexrangespec *spec) {
    return spec->maxex ?
        (sdscmplex(value,spec->max) < 0) :
        (sdscmplex(value,spec->max) <= 0);
}

/* Returns if there is a part of the zset is in the lex range. */
int zslIsInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;

    /* Test for ranges that will always be empty. */
    int cmp = sdscmplex(range->min,range->max);
    if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex)))
        return 0;
    x = zsl->tail;
    if (x == NULL || !zslLexValueGteMin(x->ele,range))
        return 0;
    x = zsl->header->level[0].forward;
    if (x == NULL || !zslLexValueLteMax(x->ele,range))
        return 0;
    return 1;
}

/* Find the first node that is contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    if (!zslIsInLexRange(zsl,range)) return NULL;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *OUT* of range. */
        while (x->level[i].forward &&
            !zslLexValueGteMin(x->level[i].forward->ele,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so the next node cannot be NULL. */
    x = x->level[0].forward;
    serverAssert(x != NULL);

    /* Check if score <= max. */
    if (!zslLexValueLteMax(x->ele,range)) return NULL;
    return x;
}

/* Find the last node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. */
zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    if (!zslIsInLexRange(zsl,range)) return NULL;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *IN* range. */
        while (x->level[i].forward &&
            zslLexValueLteMax(x->level[i].forward->ele,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so this node cannot be NULL. */
    serverAssert(x != NULL);

    /* Check if score >= min. */
    if (!zslLexValueGteMin(x->ele,range)) return NULL;
    return x;
}

/*-----------------------------------------------------------------------------
 * Ziplist-backed sorted set API
 *----------------------------------------------------------------------------*/

double zzlStrtod(unsigned char *vstr, unsigned int vlen) {
    char buf[128];
    if (vlen > sizeof(buf))
        vlen = sizeof(buf);
    memcpy(buf,vstr,vlen);
    buf[vlen] = '\0';
    return strtod(buf,NULL);
 }

double zzlGetScore(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    double score;

    serverAssert(sptr != NULL);
    serverAssert(ziplistGet(sptr,&vstr,&vlen,&vlong));

    if (vstr) {
        score = zzlStrtod(vstr,vlen);
    } else {
        score = vlong;
    }

    return score;
}

/* Return a ziplist element as an SDS string. */
sds ziplistGetObject(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;

    serverAssert(sptr != NULL);
    serverAssert(ziplistGet(sptr,&vstr,&vlen,&vlong));

    if (vstr) {
        return sdsnewlen((char*)vstr,vlen);
    } else {
        return sdsfromlonglong(vlong);
    }
}

/* Compare element in sorted set with given element. */
int zzlCompareElements(unsigned char *eptr, unsigned char *cstr, unsigned int clen) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    unsigned char vbuf[32];
    int minlen, cmp;

    serverAssert(ziplistGet(eptr,&vstr,&vlen,&vlong));
    if (vstr == NULL) {
        /* Store string representation of long long in buf. */
        vlen = ll2string((char*)vbuf,sizeof(vbuf),vlong);
        vstr = vbuf;
    }

    minlen = (vlen < clen) ? vlen : clen;
    cmp = memcmp(vstr,cstr,minlen);
    if (cmp == 0) return vlen-clen;
    return cmp;
}

unsigned int zzlLength(unsigned char *zl) {
    return ziplistLen(zl)/2;
}

/* Move to next entry based on the values in eptr and sptr. Both are set to
 * NULL when there is no next entry. */
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    _eptr = ziplistNext(zl,*sptr);
    if (_eptr != NULL) {
        _sptr = ziplistNext(zl,_eptr);
        serverAssert(_sptr != NULL);
    } else {
        /* No next entry. */
        _sptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/* Move to the previous entry based on the values in eptr and sptr. Both are
 * set to NULL when there is no next entry. */
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    _sptr = ziplistPrev(zl,*eptr);
    if (_sptr != NULL) {
        _eptr = ziplistPrev(zl,_sptr);
        serverAssert(_eptr != NULL);
    } else {
        /* No previous entry. */
        _eptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
int zzlIsInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *p;
    double score;

    /* Test for ranges that will always be empty. */
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))
        return 0;

    p = ziplistIndex(zl,-1); /* Last score. */
    if (p == NULL) return 0; /* Empty sorted set */
    score = zzlGetScore(p);
    if (!zslValueGteMin(score,range))
        return 0;

    p = ziplistIndex(zl,1); /* First score. */
    serverAssert(p != NULL);
    score = zzlGetScore(p);
    if (!zslValueLteMax(score,range))
        return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;
    double score;

    /* If everything is out of range, return early. */
    if (!zzlIsInRange(zl,range)) return NULL;

    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        score = zzlGetScore(sptr);
        if (zslValueGteMin(score,range)) {
            /* Check if score <= max. */
            if (zslValueLteMax(score,range))
                return eptr;
            return NULL;
        }

        /* Move to next element. */
        eptr = ziplistNext(zl,sptr);
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,-2), *sptr;
    double score;

    /* If everything is out of range, return early. */
    if (!zzlIsInRange(zl,range)) return NULL;

    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        score = zzlGetScore(sptr);
        if (zslValueLteMax(score,range)) {
            /* Check if score >= min. */
            if (zslValueGteMin(score,range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        sptr = ziplistPrev(zl,eptr);
        if (sptr != NULL)
            serverAssert((eptr = ziplistPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec) {
    sds value = ziplistGetObject(p);
    int res = zslLexValueGteMin(value,spec);
    sdsfree(value);
    return res;
}

int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec) {
    sds value = ziplistGetObject(p);
    int res = zslLexValueLteMax(value,spec);
    sdsfree(value);
    return res;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
int zzlIsInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *p;

    /* Test for ranges that will always be empty. */
    int cmp = sdscmplex(range->min,range->max);
    if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex)))
        return 0;

    p = ziplistIndex(zl,-2); /* Last element. */
    if (p == NULL) return 0;
    if (!zzlLexValueGteMin(p,range))
        return 0;

    p = ziplistIndex(zl,0); /* First element. */
    serverAssert(p != NULL);
    if (!zzlLexValueLteMax(p,range))
        return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;

    /* If everything is out of range, return early. */
    if (!zzlIsInLexRange(zl,range)) return NULL;

    while (eptr != NULL) {
        if (zzlLexValueGteMin(eptr,range)) {
            /* Check if score <= max. */
            if (zzlLexValueLteMax(eptr,range))
                return eptr;
            return NULL;
        }

        /* Move to next element. */
        sptr = ziplistNext(zl,eptr); /* This element score. Skip it. */
        serverAssert(sptr != NULL);
        eptr = ziplistNext(zl,sptr); /* Next element. */
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,-2), *sptr;

    /* If everything is out of range, return early. */
    if (!zzlIsInLexRange(zl,range)) return NULL;

    while (eptr != NULL) {
        if (zzlLexValueLteMax(eptr,range)) {
            /* Check if score >= min. */
            if (zzlLexValueGteMin(eptr,range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        sptr = ziplistPrev(zl,eptr);
        if (sptr != NULL)
            serverAssert((eptr = ziplistPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

unsigned char *zzlFind(unsigned char *zl, sds ele, double *score) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;

    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        if (ziplistCompare(eptr,(unsigned char*)ele,sdslen(ele))) {
            /* Matching element, pull out score. */
            if (score != NULL) *score = zzlGetScore(sptr);
            return eptr;
        }

        /* Move to next element. */
        eptr = ziplistNext(zl,sptr);
    }
    return NULL;
}

/* Delete (element,score) pair from ziplist. Use local copy of eptr because we
 * don't want to modify the one given as argument. */
unsigned char *zzlDelete(unsigned char *zl, unsigned char *eptr) {
    unsigned char *p = eptr;

    /* TODO: add function to ziplist API to delete N elements from offset. */
    zl = ziplistDelete(zl,&p);
    zl = ziplistDelete(zl,&p);
    return zl;
}

unsigned char *zzlInsertAt(unsigned char *zl, unsigned char *eptr, sds ele, double score) {
    unsigned char *sptr;
    char scorebuf[128];
    int scorelen;
    size_t offset;

    scorelen = d2string(scorebuf,sizeof(scorebuf),score);
    if (eptr == NULL) {
        zl = ziplistPush(zl,(unsigned char*)ele,sdslen(ele),ZIPLIST_TAIL);
        zl = ziplistPush(zl,(unsigned char*)scorebuf,scorelen,ZIPLIST_TAIL);
    } else {
        /* Keep offset relative to zl, as it might be re-allocated. */
        offset = eptr-zl;
        zl = ziplistInsert(zl,eptr,(unsigned char*)ele,sdslen(ele));
        eptr = zl+offset;

        /* Insert score after the element. */
        serverAssert((sptr = ziplistNext(zl,eptr)) != NULL);
        zl = ziplistInsert(zl,sptr,(unsigned char*)scorebuf,scorelen);
    }
    return zl;
}

/* Insert (element,score) pair in ziplist. This function assumes the element is
 * not yet present in the list. */
unsigned char *zzlInsert(unsigned char *zl, sds ele, double score) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;
    double s;

    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);
        s = zzlGetScore(sptr);

        if (s > score) {
            /* First element with score larger than score for element to be
             * inserted. This means we should take its spot in the list to
             * maintain ordering. */
            zl = zzlInsertAt(zl,eptr,ele,score);
            break;
        } else if (s == score) {
            /* Ensure lexicographical ordering for elements. */
            if (zzlCompareElements(eptr,(unsigned char*)ele,sdslen(ele)) > 0) {
                zl = zzlInsertAt(zl,eptr,ele,score);
                break;
            }
        }

        /* Move to next element. */
        eptr = ziplistNext(zl,sptr);
    }

    /* Push on tail of list when it was not yet inserted. */
    if (eptr == NULL)
        zl = zzlInsertAt(zl,NULL,ele,score);
    return zl;
}

unsigned char *zzlDeleteRangeByScore(unsigned char *zl, zrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    double score;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    eptr = zzlFirstInRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the ziplist is deleted, eptr will point to the sentinel
     * byte and ziplistNext will return NULL. */
    while ((sptr = ziplistNext(zl,eptr)) != NULL) {
        score = zzlGetScore(sptr);
        if (zslValueLteMax(score,range)) {
            /* Delete both the element and the score. */
            zl = ziplistDelete(zl,&eptr);
            zl = ziplistDelete(zl,&eptr);
            num++;
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

unsigned char *zzlDeleteRangeByLex(unsigned char *zl, zlexrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    eptr = zzlFirstInLexRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the ziplist is deleted, eptr will point to the sentinel
     * byte and ziplistNext will return NULL. */
    while ((sptr = ziplistNext(zl,eptr)) != NULL) {
        if (zzlLexValueLteMax(eptr,range)) {
            /* Delete both the element and the score. */
            zl = ziplistDelete(zl,&eptr);
            zl = ziplistDelete(zl,&eptr);
            num++;
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
unsigned char *zzlDeleteRangeByRank(unsigned char *zl, unsigned int start, unsigned int end, unsigned long *deleted) {
    unsigned int num = (end-start)+1;
    if (deleted) *deleted = num;
    zl = ziplistDeleteRange(zl,2*(start-1),2*num);
    return zl;
}

/*-----------------------------------------------------------------------------
 * Common sorted set API
 *----------------------------------------------------------------------------*/

unsigned long zsetLength(const robj *zobj) {
    unsigned long length = 0;
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        length = zzlLength(zobj->ptr);
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        length = ((const zset*)zobj->ptr)->zsl->length;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return length;
}

void zsetConvert(robj *zobj, int encoding) {
    zset *zs;
    zskiplistNode *node, *next;
    sds ele;
    double score;

    if (zobj->encoding == encoding) return;
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        if (encoding != OBJ_ENCODING_SKIPLIST)
            serverPanic("Unknown target encoding");

        zs = zmalloc(sizeof(*zs));
        zs->dict = dictCreate(&zsetDictType,NULL);
        zs->zsl = zslCreate();

        eptr = ziplistIndex(zl,0);
        serverAssertWithInfo(NULL,zobj,eptr != NULL);
        sptr = ziplistNext(zl,eptr);
        serverAssertWithInfo(NULL,zobj,sptr != NULL);

        while (eptr != NULL) {
            score = zzlGetScore(sptr);
            serverAssertWithInfo(NULL,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));
            if (vstr == NULL)
                ele = sdsfromlonglong(vlong);
            else
                ele = sdsnewlen((char*)vstr,vlen);

            node = zslInsert(zs->zsl,score,ele);
            serverAssert(dictAdd(zs->dict,ele,&node->score) == DICT_OK);
            zzlNext(zl,&eptr,&sptr);
        }

        zfree(zobj->ptr);
        zobj->ptr = zs;
        zobj->encoding = OBJ_ENCODING_SKIPLIST;
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        unsigned char *zl = ziplistNew();

        if (encoding != OBJ_ENCODING_ZIPLIST)
            serverPanic("Unknown target encoding");

        /* Approach similar to zslFree(), since we want to free the skiplist at
         * the same time as creating the ziplist. */
        zs = zobj->ptr;
        dictRelease(zs->dict);
        node = zs->zsl->header->level[0].forward;
        zfree(zs->zsl->header);
        zfree(zs->zsl);

        while (node) {
            zl = zzlInsertAt(zl,NULL,node->ele,node->score);
            next = node->level[0].forward;
            zslFreeNode(node);
            node = next;
        }

        zfree(zs);
        zobj->ptr = zl;
        zobj->encoding = OBJ_ENCODING_ZIPLIST;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}


/* Convert the sorted set object into a ziplist if it is not already a ziplist
 * and if the number of elements and the maximum element size and total elements size
 * are within the expected ranges. */
void zsetConvertToZiplistIfNeeded(robj *zobj, size_t maxelelen, size_t totelelen) {
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) return;
    zset *zset = zobj->ptr;
    /*
    if (zset->zsl->length <= server.zset_max_ziplist_entries &&
        maxelelen <= server.zset_max_ziplist_value &&
        ziplistSafeToAdd(NULL, totelelen))
    {
        zsetConvert(zobj,OBJ_ENCODING_ZIPLIST);
    }    
    */
}

/* Return (by reference) the score of the specified member of the sorted set
 * storing it into *score. If the element does not exist C_ERR is returned
 * otherwise C_OK is returned and *score is correctly populated.
 * If 'zobj' or 'member' is NULL, C_ERR is returned. */
int zsetScore(robj *zobj, sds member, double *score) {
    if (!zobj || !member) return C_ERR;

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        if (zzlFind(zobj->ptr, member, score) == NULL) return C_ERR;
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        dictEntry *de = dictFind(zs->dict, member);
        if (de == NULL) return C_ERR;
        *score = *(double*)dictGetVal(de);
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return C_OK;
}


/* Add a new element or update the score of an existing element in a sorted
 * set, regardless of its encoding.
 *
 * The set of flags change the command behavior. 
 *
 * The input flags are the following:
 *
 * ZADD_INCR: Increment the current element score by 'score' instead of updating
 *            the current element score. If the element does not exist, we
 *            assume 0 as previous score.
 * ZADD_NX:   Perform the operation only if the element does not exist.
 * ZADD_XX:   Perform the operation only if the element already exist.
 * ZADD_GT:   Perform the operation on existing elements only if the new score is 
 *            greater than the current score.
 * ZADD_LT:   Perform the operation on existing elements only if the new score is 
 *            less than the current score.
 *
 * When ZADD_INCR is used, the new score of the element is stored in
 * '*newscore' if 'newscore' is not NULL.
 *
 * The returned flags are the following:
 *
 * ZADD_NAN:     The resulting score is not a number.
 * ZADD_ADDED:   The element was added (not present before the call).
 * ZADD_UPDATED: The element score was updated.
 * ZADD_NOP:     No operation was performed because of NX or XX.
 *
 * Return value:
 *
 * The function returns 1 on success, and sets the appropriate flags
 * ADDED or UPDATED to signal what happened during the operation (note that
 * none could be set if we re-added an element using the same score it used
 * to have, or in the case a zero increment is used).
 *
 * The function returns 0 on error, currently only when the increment
 * produces a NAN condition, or when the 'score' value is NAN since the
 * start.
 *
 * The command as a side effect of adding a new element may convert the sorted
 * set internal encoding from ziplist to hashtable+skiplist.
 *
 * Memory management of 'ele':
 *
 * The function does not take ownership of the 'ele' SDS string, but copies
 * it if needed. */
int zsetAdd(robj *zobj, double score, sds ele, int in_flags, int *out_flags, double *newscore) {
    /* Turn options into simple to check vars. */
    int incr = (in_flags & ZADD_IN_INCR) != 0;
    int nx = (in_flags & ZADD_IN_NX) != 0;
    int xx = (in_flags & ZADD_IN_XX) != 0;
    int gt = (in_flags & ZADD_IN_GT) != 0;
    int lt = (in_flags & ZADD_IN_LT) != 0;
    *out_flags = 0; /* We'll return our response flags. */
    double curscore;

    /* NaN as input is an error regardless of all the other parameters. */
    if (isnan(score)) {
        *out_flags = ZADD_OUT_NAN;
        return 0;
    }

    /* Update the sorted set according to its encoding. */
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *eptr;

        if ((eptr = zzlFind(zobj->ptr,ele,&curscore)) != NULL) {
            /* NX? Return, same element already exists. */
            if (nx) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            /* Prepare the score for the increment if needed. */
            if (incr) {
                score += curscore;
                if (isnan(score)) {
                    *out_flags |= ZADD_OUT_NAN;
                    return 0;
                }
            }

            /* GT/LT? Only update if score is greater/less than current. */
            if ((lt && score >= curscore) || (gt && score <= curscore)) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            if (newscore) *newscore = score;

            /* Remove and re-insert when score changed. */
            if (score != curscore) {
                zobj->ptr = zzlDelete(zobj->ptr,eptr);
                zobj->ptr = zzlInsert(zobj->ptr,ele,score);
                *out_flags |= ZADD_OUT_UPDATED;
            }
            return 1;
        } else if (!xx) {
            
            /* check if the element is too large or the list
             * becomes too long *before* executing zzlInsert. */
            //delete by zhenjia.zhao
             /*
             if (zzlLength(zobj->ptr)+1 > server.zset_max_ziplist_entries ||
                sdslen(ele) > server.zset_max_ziplist_value ||
                !ziplistSafeToAdd(zobj->ptr, sdslen(ele)))
            {
                zsetConvert(zobj,OBJ_ENCODING_SKIPLIST);
            } else {
                zobj->ptr = zzlInsert(zobj->ptr,ele,score);
                if (newscore) *newscore = score;
                *out_flags |= ZADD_OUT_ADDED;
                return 1;
            } 
            //end
            */

        
        
        } else {
            *out_flags |= ZADD_OUT_NOP;
            return 1;
        }
    }

    /* Note that the above block handling ziplist would have either returned or
     * converted the key to skiplist. */
    if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplistNode *znode;
        dictEntry *de;

        de = dictFind(zs->dict,ele);
        if (de != NULL) {
            /* NX? Return, same element already exists. */
            if (nx) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            curscore = *(double*)dictGetVal(de);

            /* Prepare the score for the increment if needed. */
            if (incr) {
                score += curscore;
                if (isnan(score)) {
                    *out_flags |= ZADD_OUT_NAN;
                    return 0;
                }
            }

            /* GT/LT? Only update if score is greater/less than current. */
            if ((lt && score >= curscore) || (gt && score <= curscore)) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            if (newscore) *newscore = score;

            /* Remove and re-insert when score changes. */
            if (score != curscore) {
                znode = zslUpdateScore(zs->zsl,curscore,ele,score);
                /* Note that we did not removed the original element from
                 * the hash table representing the sorted set, so we just
                 * update the score. */
                dictGetVal(de) = &znode->score; /* Update score ptr. */
                *out_flags |= ZADD_OUT_UPDATED;
            }
            return 1;
        } else if (!xx) {
            ele = sdsdup(ele);
            znode = zslInsert(zs->zsl,score,ele);
            serverAssert(dictAdd(zs->dict,ele,&znode->score) == DICT_OK);
            *out_flags |= ZADD_OUT_ADDED;
            if (newscore) *newscore = score;
            return 1;
        } else {
            *out_flags |= ZADD_OUT_NOP;
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* Never reached. */
}

/* Deletes the element 'ele' from the sorted set encoded as a skiplist+dict,
 * returning 1 if the element existed and was deleted, 0 otherwise (the
 * element was not there). It does not resize the dict after deleting the
 * element. */
static int zsetRemoveFromSkiplist(zset *zs, sds ele) {
    dictEntry *de;
    double score;

    de = dictUnlink(zs->dict,ele);
    if (de != NULL) {
        /* Get the score in order to delete from the skiplist later. */
        score = *(double*)dictGetVal(de);

        /* Delete from the hash table and later from the skiplist.
         * Note that the order is important: deleting from the skiplist
         * actually releases the SDS string representing the element,
         * which is shared between the skiplist and the hash table, so
         * we need to delete from the skiplist as the final step. */
        dictFreeUnlinkedEntry(zs->dict,de);

        /* Delete from skiplist. */
        int retval = zslDelete(zs->zsl,score,ele,NULL);
        serverAssert(retval);

        return 1;
    }

    return 0;
}

/* Delete the element 'ele' from the sorted set, returning 1 if the element
 * existed and was deleted, 0 otherwise (the element was not there). */
int zsetDel(robj *zobj, sds ele) {
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *eptr;

        if ((eptr = zzlFind(zobj->ptr,ele,NULL)) != NULL) {
            zobj->ptr = zzlDelete(zobj->ptr,eptr);
            return 1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        if (zsetRemoveFromSkiplist(zs, ele)) {
            if (htNeedsResize(zs->dict)) dictResize(zs->dict);
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* No such element found. */
}

/* Given a sorted set object returns the 0-based rank of the object or
 * -1 if the object does not exist.
 *
 * For rank we mean the position of the element in the sorted collection
 * of elements. So the first element has rank 0, the second rank 1, and so
 * forth up to length-1 elements.
 *
 * If 'reverse' is false, the rank is returned considering as first element
 * the one with the lowest score. Otherwise if 'reverse' is non-zero
 * the rank is computed considering as element with rank 0 the one with
 * the highest score. */
long zsetRank(robj *zobj, sds ele, int reverse) {
    unsigned long llen;
    unsigned long rank;

    llen = zsetLength(zobj);

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        eptr = ziplistIndex(zl,0);
        serverAssert(eptr != NULL);
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        rank = 1;
        while(eptr != NULL) {
            if (ziplistCompare(eptr,(unsigned char*)ele,sdslen(ele)))
                break;
            rank++;
            zzlNext(zl,&eptr,&sptr);
        }

        if (eptr != NULL) {
            if (reverse)
                return llen-rank;
            else
                return rank-1;
        } else {
            return -1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        dictEntry *de;
        double score;

        de = dictFind(zs->dict,ele);
        if (de != NULL) {
            score = *(double*)dictGetVal(de);
            rank = zslGetRank(zsl,score,ele);
            /* Existing elements always have a rank. */
            serverAssert(rank != 0);
            if (reverse)
                return llen-rank;
            else
                return rank-1;
        } else {
            return -1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a sorted set object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *zsetDup(robj *o) {
    robj *zobj;
    zset *zs;
    zset *new_zs;

    serverAssert(o->type == OBJ_ZSET);

    /* Create a new sorted set object that have the same encoding as the original object's encoding */
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = o->ptr;
        size_t sz = ziplistBlobLen(zl);
        unsigned char *new_zl = zmalloc(sz);
        memcpy(new_zl, zl, sz);
        zobj = createObject(OBJ_ZSET, new_zl);
        zobj->encoding = OBJ_ENCODING_ZIPLIST;
    } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
        zobj = createZsetObject();
        zs = o->ptr;
        new_zs = zobj->ptr;
        dictExpand(new_zs->dict,dictSize(zs->dict));
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;
        sds ele;
        long llen = zsetLength(o);

        /* We copy the skiplist elements from the greatest to the
         * smallest (that's trivial since the elements are already ordered in
         * the skiplist): this improves the load process, since the next loaded
         * element will always be the smaller, so adding to the skiplist
         * will always immediately stop at the head, making the insertion
         * O(1) instead of O(log(N)). */
        ln = zsl->tail;
        while (llen--) {
            ele = ln->ele;
            sds new_ele = sdsdup(ele);
            zskiplistNode *znode = zslInsert(new_zs->zsl,ln->score,new_ele);
            dictAdd(new_zs->dict,new_ele,&znode->score);
            ln = ln->backward;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return zobj;
}

/* callback for to check the ziplist doesn't have duplicate recoreds */
static int _zsetZiplistValidateIntegrity(unsigned char *p, void *userdata) {
    struct {
        long count;
        dict *fields;
    } *data = userdata;

    /* Even records are field names, add to dict and check that's not a dup */
    if (((data->count) & 1) == 0) {
        unsigned char *str;
        unsigned int slen;
        long long vll;
        if (!ziplistGet(p, &str, &slen, &vll))
            return 0;
        sds field = str? sdsnewlen(str, slen): sdsfromlonglong(vll);;
        if (dictAdd(data->fields, field, NULL) != DICT_OK) {
            /* Duplicate, return an error */
            sdsfree(field);
            return 0;
        }
    }

    (data->count)++;
    return 1;
}

/* Validate the integrity of the data structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we scan all the entries one by one. */
int zsetZiplistValidateIntegrity(unsigned char *zl, size_t size, int deep) {
    if (!deep)
        return ziplistValidateIntegrity(zl, size, 0, NULL, NULL);

    /* Keep track of the field names to locate duplicate ones */
    struct {
        long count;
        dict *fields;
    } data = {0, dictCreate(&hashDictType, NULL)};

    int ret = ziplistValidateIntegrity(zl, size, 1, _zsetZiplistValidateIntegrity, &data);

    /* make sure we have an even number of records. */
    if (data.count & 1)
        ret = 0;

    dictRelease(data.fields);
    return ret;
}

/* Create a new sds string from the ziplist entry. */
//delete zhenjia.zhao
/*sds zsetSdsFromZiplistEntry(ziplistEntry *e) {
    return e->sval ? sdsnewlen(e->sval, e->slen) : sdsfromlonglong(e->lval);
}*/

/* Reply with bulk string from the ziplist entry. */
//delete by zhenjia.zhao
/*void zsetReplyFromZiplistEntry(client *c, ziplistEntry *e) {
    if (e->sval)
        addReplyBulkCBuffer(c, e->sval, e->slen);
    else
        addReplyBulkLongLong(c, e->lval);
}*/


/* Return random element from a non empty zset.
 * 'key' and 'val' will be set to hold the element.
 * The memory in `key` is not to be freed or modified by the caller.
 * 'score' can be NULL in which case it's not extracted. */
void zsetTypeRandomElement(robj *zsetobj, unsigned long zsetsize, ziplistEntry *key, double *score) {
    if (zsetobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zsetobj->ptr;
        dictEntry *de = dictGetFairRandomKey(zs->dict);
        sds s = dictGetKey(de);
        key->sval = (unsigned char*)s;
        key->slen = sdslen(s);
        if (score)
            *score = *(double*)dictGetVal(de);
    } else if (zsetobj->encoding == OBJ_ENCODING_ZIPLIST) {
        ziplistEntry val;
        ziplistRandomPair(zsetobj->ptr, zsetsize, key, &val);
        if (score) {
            if (val.sval) {
                *score = zzlStrtod(val.sval,val.slen);
            } else {
                *score = (double)val.lval;
            }
        }
    } else {
        serverPanic("Unknown zset encoding");
    }
}
