/* This file is part of Pazpar2.
   Copyright (C) 2006-2012 Index Data

Pazpar2 is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.

Pazpar2 is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "relevance.h"
#include "session.h"

struct relevance
{
    int *doc_frequency_vec;
    int vec_len;
    struct word_entry *entries;
    pp2_charset_token_t prt;
    NMEM nmem;
};

struct word_entry {
    const char *norm_str;
    int termno;
    char *ccl_field;
    struct word_entry *next;
};

static int word_entry_match(struct word_entry *entries, const char *norm_str,
                            const char *frank, int *local_mult)
{
    for (; entries; entries = entries->next)
    {
        if (!strcmp(norm_str, entries->norm_str))
        {
            const char *cp = 0;
            if (frank && (cp = strchr(frank, ' ')))
            {
                if ((cp - frank) == strlen(entries->ccl_field) &&
                    memcmp(entries->ccl_field, frank, cp - frank) == 0)
                    *local_mult = atoi(cp + 1);
            }
            return entries->termno;
        }
    }
    return 0;
}

void relevance_countwords(struct relevance *r, struct record_cluster *cluster,
                          const char *words, int multiplier, const char *name,
                          const char *frank)
{
    int *mult = cluster->term_frequency_vec_tmp;
    const char *norm_str;
    int i, length = 0;
    pp2_charset_token_first(r->prt, words, 0);
    for (i = 1; i < r->vec_len; i++)
        mult[i] = 0;

    while ((norm_str = pp2_charset_token_next(r->prt)))
    {
        int local_mult = multiplier;
        int res = word_entry_match(r->entries, norm_str, frank, &local_mult);
        if (res)
        {
            assert(res < r->vec_len);
            mult[res] += local_mult;
        }
        length++;
    }

    for (i = 1; i < r->vec_len; i++)
    {
        if (length > 0) /* only add if non-empty */
            cluster->term_frequency_vecf[i] += (double) mult[i] / length;
        cluster->term_frequency_vec[i] += mult[i];
    }

    cluster->term_frequency_vec[0] += length;
}

static void pull_terms(struct relevance *res, struct ccl_rpn_node *n)
{
    char **words;
    int numwords;
    char *ccl_field;
    int i;

    switch (n->kind)
    {
    case CCL_RPN_AND:
    case CCL_RPN_OR:
    case CCL_RPN_NOT:
    case CCL_RPN_PROX:
        pull_terms(res, n->u.p[0]);
        pull_terms(res, n->u.p[1]);
        break;
    case CCL_RPN_TERM:
        nmem_strsplit(res->nmem, " ", n->u.t.term, &words, &numwords);
        for (i = 0; i < numwords; i++)
        {
            const char *norm_str;

            ccl_field = nmem_strdup_null(res->nmem, n->u.t.qual);

            pp2_charset_token_first(res->prt, words[i], 0);
            while ((norm_str = pp2_charset_token_next(res->prt)))
            {
                struct word_entry **e = &res->entries;
                while (*e)
                    e = &(*e)->next;
                *e = nmem_malloc(res->nmem, sizeof(**e));
                (*e)->norm_str = nmem_strdup(res->nmem, norm_str);
                (*e)->ccl_field = ccl_field;
                (*e)->termno = res->vec_len++;
                (*e)->next = 0;
            }
        }
        break;
    default:
        break;
    }
}

struct relevance *relevance_create_ccl(pp2_charset_fact_t pft,
                                       struct ccl_rpn_node *query)
{
    NMEM nmem = nmem_create();
    struct relevance *res = nmem_malloc(nmem, sizeof(*res));
    int i;

    res->nmem = nmem;
    res->entries = 0;
    res->vec_len = 1;
    res->prt = pp2_charset_token_create(pft, "relevance");

    pull_terms(res, query);

    res->doc_frequency_vec = nmem_malloc(nmem, res->vec_len * sizeof(int));
    for (i = 0; i < res->vec_len; i++)
        res->doc_frequency_vec[i] = 0;
    return res;
}

void relevance_destroy(struct relevance **rp)
{
    if (*rp)
    {
        pp2_charset_token_destroy((*rp)->prt);
        nmem_destroy((*rp)->nmem);
        *rp = 0;
    }
}

void relevance_newrec(struct relevance *r, struct record_cluster *rec)
{
    if (!rec->term_frequency_vec)
    {
        int i;

        // term frequency [1,..] . [0] is total length of all fields
        rec->term_frequency_vec =
            nmem_malloc(r->nmem,
                        r->vec_len * sizeof(*rec->term_frequency_vec));
        for (i = 0; i < r->vec_len; i++)
            rec->term_frequency_vec[i] = 0;

        // term frequency divided by length of field [1,...]
        rec->term_frequency_vecf =
            nmem_malloc(r->nmem,
                        r->vec_len * sizeof(*rec->term_frequency_vecf));
        for (i = 0; i < r->vec_len; i++)
            rec->term_frequency_vecf[i] = 0.0;

        // for relevance_countwords (so we don't have to xmalloc/xfree)
        rec->term_frequency_vec_tmp =
            nmem_malloc(r->nmem,
                        r->vec_len * sizeof(*rec->term_frequency_vec_tmp));
    }
}

void relevance_donerecord(struct relevance *r, struct record_cluster *cluster)
{
    int i;

    for (i = 1; i < r->vec_len; i++)
        if (cluster->term_frequency_vec[i] > 0)
            r->doc_frequency_vec[i]++;

    r->doc_frequency_vec[0]++;
}

// Prepare for a relevance-sorted read
void relevance_prepare_read(struct relevance *rel, struct reclist *reclist)
{
    int i;
    float *idfvec = xmalloc(rel->vec_len * sizeof(float));

    reclist_enter(reclist);
    // Calculate document frequency vector for each term.
    for (i = 1; i < rel->vec_len; i++)
    {
        if (!rel->doc_frequency_vec[i])
            idfvec[i] = 0;
        else
        {
            // This conditional may be terribly wrong
            // It was there to address the situation where vec[0] == vec[i]
            // which leads to idfvec[i] == 0... not sure about this
            // Traditional TF-IDF may assume that a word that occurs in every
            // record is irrelevant, but this is actually something we will
            // see a lot
            if ((idfvec[i] = log((float) rel->doc_frequency_vec[0] /
                            rel->doc_frequency_vec[i])) < 0.0000001)
                idfvec[i] = 1;
        }
    }
    // Calculate relevance for each document
    while (1)
    {
        int t;
        int relevance = 0;
        struct record_cluster *rec = reclist_read_record(reclist);
        if (!rec)
            break;
        for (t = 1; t < rel->vec_len; t++)
        {
            float termfreq = (float) rec->term_frequency_vecf[t];
            relevance += 100000 * (termfreq * idfvec[t] + 0.0000005);
        }
        rec->relevance_score = relevance;
    }
    reclist_leave(reclist);
    xfree(idfvec);
}

/*
 * Local variables:
 * c-basic-offset: 4
 * c-file-style: "Stroustrup"
 * indent-tabs-mode: nil
 * End:
 * vim: shiftwidth=4 tabstop=8 expandtab
 */

