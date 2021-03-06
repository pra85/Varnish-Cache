/*-
 * Copyright (c) 2007-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Dag-Erling Smørgav <des@des.no>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * STEVEDORE: one who works at or is responsible for loading and
 * unloading ships in port.  Example: "on the wharves, stevedores were
 * unloading cargo from the far corners of the world." Origin: Spanish
 * estibador, from estibar to pack.  First Known Use: 1788
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "cache/cache.h"

#include "storage/storage.h"
#include "vrt.h"
#include "vrt_obj.h"

static const struct stevedore * volatile stv_next;

/*--------------------------------------------------------------------
 */

struct lru *
LRU_Alloc(void)
{
	struct lru *l;

	ALLOC_OBJ(l, LRU_MAGIC);
	AN(l);
	VTAILQ_INIT(&l->lru_head);
	Lck_New(&l->mtx, lck_lru);
	return (l);
}

void
LRU_Free(struct lru *lru)
{
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);
	Lck_Delete(&lru->mtx);
	FREE_OBJ(lru);
}

/*--------------------------------------------------------------------
 * XXX: trust pointer writes to be atomic
 */

static struct stevedore *
stv_pick_stevedore(struct vsl_log *vsl, const char **hint)
{
	struct stevedore *stv;

	AN(hint);
	if (*hint != NULL && **hint != '\0') {
		VTAILQ_FOREACH(stv, &stv_stevedores, list) {
			if (!strcmp(stv->ident, *hint))
				return (stv);
		}
		if (!strcmp(TRANSIENT_STORAGE, *hint))
			return (stv_transient);

		/* Hint was not valid, nuke it */
		VSLb(vsl, SLT_Debug, "Storage hint not usable");
		*hint = NULL;
	}
	if (stv_next == NULL)
		return (stv_transient);
	/* pick a stevedore and bump the head along */
	stv = VTAILQ_NEXT(stv_next, list);
	if (stv == NULL)
		stv = VTAILQ_FIRST(&stv_stevedores);
	AN(stv);
	AN(stv->name);
	stv_next = stv;
	return (stv);
}


/*-------------------------------------------------------------------
 * Allocate storage for an object, based on the header information.
 * XXX: If we know (a hint of) the length, we could allocate space
 * XXX: for the body in the same allocation while we are at it.
 */

int
STV_NewObject(struct worker *wrk, struct objcore *oc,
    const char *hint, unsigned wsl)
{
	struct stevedore *stv, *stv0;
	int j;

	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	assert(wsl > 0);

	stv = stv0 = stv_pick_stevedore(wrk->vsl, &hint);
	AN(stv->allocobj);
	j = stv->allocobj(wrk, stv, oc, wsl, 0);
	if (j == 0 && hint == NULL) {
		do {
			stv = stv_pick_stevedore(wrk->vsl, &hint);
			AN(stv->allocobj);
			j = stv->allocobj(wrk, stv, oc, wsl, 0);
		} while (j == 0 && stv != stv0);
	}
	if (j == 0 && cache_param->nuke_limit > 0) {
		/* no luck; try to free some space and keep trying */
		j = stv->allocobj(wrk, stv, oc, wsl, cache_param->nuke_limit);
	}

	if (j == 0)
		return (0);

	wrk->stats->n_object++;
	VSLb(wrk->vsl, SLT_Storage, "%s %s",
	    oc->stobj->stevedore->name, oc->stobj->stevedore->ident);
	return (1);
}

/*-------------------------------------------------------------------*/

void
STV_open(void)
{
	struct stevedore *stv;

	VTAILQ_FOREACH(stv, &stv_stevedores, list) {
		stv->lru = LRU_Alloc();
		if (stv->open != NULL)
			stv->open(stv);
	}
	stv = stv_transient;
	if (stv->open != NULL) {
		stv->lru = LRU_Alloc();
		stv->open(stv);
	}
	stv_next = VTAILQ_FIRST(&stv_stevedores);
}

void
STV_close(void)
{
	struct stevedore *stv;

	/* Signal intent to close */
	VTAILQ_FOREACH(stv, &stv_stevedores, list)
		if (stv->signal_close != NULL)
			stv->signal_close(stv);
	stv = stv_transient;
	if (stv->signal_close != NULL)
		stv->signal_close(stv);

	/* Close each in turn */
	VTAILQ_FOREACH(stv, &stv_stevedores, list)
		if (stv->close != NULL)
			stv->close(stv);
	stv = stv_transient;
	if (stv->close != NULL)
		stv->close(stv);
}

/*-------------------------------------------------------------------
 * Notify the stevedores of BAN related events. A non-zero return
 * value indicates that the stevedore is unable to persist the
 * event.
 */

int
STV_BanInfo(enum baninfo event, const uint8_t *ban, unsigned len)
{
	struct stevedore *stv;
	int r = 0;

	VTAILQ_FOREACH(stv, &stv_stevedores, list)
		if (stv->baninfo != NULL)
			r |= stv->baninfo(stv, event, ban, len);

	return (r);
}

/*-------------------------------------------------------------------
 * Export a complete ban list to the stevedores for persistence.
 * The stevedores should clear any previous ban lists and replace
 * them with this list.
 */

void
STV_BanExport(const uint8_t *bans, unsigned len)
{
	struct stevedore *stv;

	VTAILQ_FOREACH(stv, &stv_stevedores, list)
		if (stv->banexport != NULL)
			stv->banexport(stv, bans, len);
}

/*--------------------------------------------------------------------
 * VRT functions for stevedores
 */

static const struct stevedore *
stv_find(const char *nm)
{
	const struct stevedore *stv;

	VTAILQ_FOREACH(stv, &stv_stevedores, list)
		if (!strcmp(stv->ident, nm))
			return (stv);
	if (!strcmp(TRANSIENT_STORAGE, nm))
		return (stv_transient);
	return (NULL);
}

int
VRT_Stv(const char *nm)
{

	if (stv_find(nm) != NULL)
		return (1);
	return (0);
}

#define VRTSTVVAR(nm, vtype, ctype, dval)	\
ctype						\
VRT_Stv_##nm(const char *nm)			\
{						\
	const struct stevedore *stv;		\
						\
	stv = stv_find(nm);			\
	if (stv == NULL)			\
		return (dval);			\
	if (stv->var_##nm == NULL)		\
		return (dval);			\
	return (stv->var_##nm(stv));		\
}

#include "tbl/vrt_stv_var.h"
#undef VRTSTVVAR
