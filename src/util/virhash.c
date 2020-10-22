/*
 * virhash.c: chained hash tables
 *
 * Reference: Your favorite introductory book on algorithms
 *
 * Copyright (C) 2005-2014 Red Hat, Inc.
 * Copyright (C) 2000 Bjorn Reese and Daniel Veillard.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS AND
 * CONTRIBUTORS ACCEPT NO RESPONSIBILITY IN ANY CONCEIVABLE MANNER.
 */

#include <config.h>


#include "virerror.h"
#include "virhash.h"
#include "viralloc.h"
#include "virlog.h"
#include "virhashcode.h"
#include "virrandom.h"
#include "virstring.h"
#include "virobject.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("util.hash");

#define MAX_HASH_LEN 8

/* #define DEBUG_GROW */

/*
 * A single entry in the hash table
 */
typedef struct _virHashEntry virHashEntry;
typedef virHashEntry *virHashEntryPtr;
struct _virHashEntry {
    struct _virHashEntry *next;
    char *name;
    void *payload;
};

/*
 * The entire hash table
 */
struct _virHashTable {
    virHashEntryPtr *table;
    uint32_t seed;
    size_t size;
    size_t nbElems;
    virHashDataFree dataFree;
};

struct _virHashAtomic {
    virObjectLockable parent;
    virHashTablePtr hash;
};

static virClassPtr virHashAtomicClass;
static void virHashAtomicDispose(void *obj);

static int virHashAtomicOnceInit(void)
{
    if (!VIR_CLASS_NEW(virHashAtomic, virClassForObjectLockable()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virHashAtomic);

static size_t
virHashComputeKey(const virHashTable *table, const char *name)
{
    uint32_t value = virHashCodeGen(name, strlen(name), table->seed);
    return value % table->size;
}


/**
 * virHashNew:
 * @dataFree: callback to free data
 *
 * Create a new virHashTablePtr.
 *
 * Returns the newly created object.
 */
virHashTablePtr
virHashNew(virHashDataFree dataFree)
{
    virHashTablePtr table = NULL;

    table = g_new0(virHashTable, 1);

    table->seed = virRandomBits(32);
    table->size = 32;
    table->nbElems = 0;
    table->dataFree = dataFree;

    table->table = g_new0(virHashEntryPtr, table->size);

    return table;
}


virHashAtomicPtr
virHashAtomicNew(virHashDataFree dataFree)
{
    virHashAtomicPtr hash;

    if (virHashAtomicInitialize() < 0)
        return NULL;

    if (!(hash = virObjectLockableNew(virHashAtomicClass)))
        return NULL;

    if (!(hash->hash = virHashNew(dataFree))) {
        virObjectUnref(hash);
        return NULL;
    }
    return hash;
}


static void
virHashAtomicDispose(void *obj)
{
    virHashAtomicPtr hash = obj;

    virHashFree(hash->hash);
}


/**
 * virHashGrow:
 * @table: the hash table
 * @size: the new size of the hash table
 *
 * resize the hash table
 *
 * Returns 0 in case of success, -1 in case of failure
 */
static int
virHashGrow(virHashTablePtr table, size_t size)
{
    size_t oldsize, i;
    virHashEntryPtr *oldtable;

#ifdef DEBUG_GROW
    size_t nbElem = 0;
#endif

    if (table == NULL)
        return -1;
    if (size < 8)
        return -1;
    if (size > 8 * 2048)
        return -1;

    oldsize = table->size;
    oldtable = table->table;
    if (oldtable == NULL)
        return -1;

    table->table = g_new0(virHashEntryPtr, size);
    table->size = size;

    for (i = 0; i < oldsize; i++) {
        virHashEntryPtr iter = oldtable[i];
        while (iter) {
            virHashEntryPtr next = iter->next;
            size_t key = virHashComputeKey(table, iter->name);

            iter->next = table->table[key];
            table->table[key] = iter;

#ifdef DEBUG_GROW
            nbElem++;
#endif
            iter = next;
        }
    }

    VIR_FREE(oldtable);

#ifdef DEBUG_GROW
    VIR_DEBUG("virHashGrow : from %d to %d, %ld elems", oldsize,
              size, nbElem);
#endif

    return 0;
}

/**
 * virHashFree:
 * @table: the hash table
 *
 * Free the hash @table and its contents. The userdata is
 * deallocated with function provided at creation time.
 */
void
virHashFree(virHashTablePtr table)
{
    size_t i;

    if (table == NULL)
        return;

    for (i = 0; i < table->size; i++) {
        virHashEntryPtr iter = table->table[i];
        while (iter) {
            virHashEntryPtr next = iter->next;

            if (table->dataFree)
                table->dataFree(iter->payload);
            g_free(iter->name);
            VIR_FREE(iter);
            iter = next;
        }
    }

    VIR_FREE(table->table);
    VIR_FREE(table);
}

static int
virHashAddOrUpdateEntry(virHashTablePtr table, const char *name,
                        void *userdata,
                        bool is_update)
{
    size_t key, len = 0;
    virHashEntryPtr entry;
    virHashEntryPtr last = NULL;

    if ((table == NULL) || (name == NULL))
        return -1;

    key = virHashComputeKey(table, name);

    /* Check for duplicate entry */
    for (entry = table->table[key]; entry; entry = entry->next) {
        if (STREQ(entry->name, name)) {
            if (is_update) {
                if (table->dataFree)
                    table->dataFree(entry->payload);
                entry->payload = userdata;
                return 0;
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Duplicate hash table key '%s'"), name);
                return -1;
            }
        }
        last = entry;
        len++;
    }

    entry = g_new0(virHashEntry, 1);
    entry->name = g_strdup(name);
    entry->payload = userdata;

    if (last)
        last->next = entry;
    else
        table->table[key] = entry;

    table->nbElems++;

    if (len > MAX_HASH_LEN)
        virHashGrow(table, MAX_HASH_LEN * table->size);

    return 0;
}

/**
 * virHashAddEntry:
 * @table: the hash table
 * @name: the name of the userdata
 * @userdata: a pointer to the userdata
 *
 * Add the @userdata to the hash @table. This can later be retrieved
 * by using @name. Duplicate entries generate errors.
 *
 * Returns 0 the addition succeeded and -1 in case of error.
 */
int
virHashAddEntry(virHashTablePtr table, const char *name, void *userdata)
{
    return virHashAddOrUpdateEntry(table, name, userdata, false);
}

/**
 * virHashUpdateEntry:
 * @table: the hash table
 * @name: the name of the userdata
 * @userdata: a pointer to the userdata
 *
 * Add the @userdata to the hash @table. This can later be retrieved
 * by using @name. Existing entry for this tuple
 * will be removed and freed with @f if found.
 *
 * Returns 0 the addition succeeded and -1 in case of error.
 */
int
virHashUpdateEntry(virHashTablePtr table, const char *name,
                   void *userdata)
{
    return virHashAddOrUpdateEntry(table, name, userdata, true);
}

int
virHashAtomicUpdate(virHashAtomicPtr table,
                    const char *name,
                    void *userdata)
{
    int ret;

    virObjectLock(table);
    ret = virHashAddOrUpdateEntry(table->hash, name, userdata, true);
    virObjectUnlock(table);

    return ret;
}


static virHashEntryPtr
virHashGetEntry(const virHashTable *table,
                const char *name)
{
    size_t key;
    virHashEntryPtr entry;

    if (!table || !name)
        return NULL;

    key = virHashComputeKey(table, name);
    for (entry = table->table[key]; entry; entry = entry->next) {
        if (STREQ(entry->name, name))
            return entry;
    }

    return NULL;
}


/**
 * virHashLookup:
 * @table: the hash table
 * @name: the name of the userdata
 *
 * Find the userdata specified by @name
 *
 * Returns a pointer to the userdata
 */
void *
virHashLookup(virHashTablePtr table,
              const char *name)
{
    virHashEntryPtr entry = virHashGetEntry(table, name);

    if (!entry)
        return NULL;

    return entry->payload;
}


/**
 * virHashHasEntry:
 * @table: the hash table
 * @name: the name of the userdata
 *
 * Find whether entry specified by @name exists.
 *
 * Returns true if the entry exists and false otherwise
 */
bool
virHashHasEntry(virHashTablePtr table,
                const char *name)
{
    return !!virHashGetEntry(table, name);
}


/**
 * virHashSteal:
 * @table: the hash table
 * @name: the name of the userdata
 *
 * Find the userdata specified by @name
 * and remove it from the hash without freeing it.
 *
 * Returns a pointer to the userdata
 */
void *virHashSteal(virHashTablePtr table, const char *name)
{
    void *data = virHashLookup(table, name);
    if (data) {
        virHashDataFree dataFree = table->dataFree;
        table->dataFree = NULL;
        virHashRemoveEntry(table, name);
        table->dataFree = dataFree;
    }
    return data;
}

void *
virHashAtomicSteal(virHashAtomicPtr table,
                   const char *name)
{
    void *data;

    virObjectLock(table);
    data = virHashSteal(table->hash, name);
    virObjectUnlock(table);

    return data;
}


/**
 * virHashSize:
 * @table: the hash table
 *
 * Query the number of elements installed in the hash @table.
 *
 * Returns the number of elements in the hash table or
 * -1 in case of error
 */
ssize_t
virHashSize(virHashTablePtr table)
{
    if (table == NULL)
        return -1;
    return table->nbElems;
}


/**
 * virHashRemoveEntry:
 * @table: the hash table
 * @name: the name of the userdata
 *
 * Find the userdata specified by the @name and remove
 * it from the hash @table. Existing userdata for this tuple will be removed
 * and freed with @f.
 *
 * Returns 0 if the removal succeeded and -1 in case of error or not found.
 */
int
virHashRemoveEntry(virHashTablePtr table, const char *name)
{
    virHashEntryPtr entry;
    virHashEntryPtr *nextptr;

    if (table == NULL || name == NULL)
        return -1;

    nextptr = table->table + virHashComputeKey(table, name);
    for (entry = *nextptr; entry; entry = entry->next) {
        if (STREQ(entry->name, name)) {
            if (table->dataFree)
                table->dataFree(entry->payload);
            g_free(entry->name);
            *nextptr = entry->next;
            VIR_FREE(entry);
            table->nbElems--;
            return 0;
        }
        nextptr = &entry->next;
    }

    return -1;
}


/**
 * virHashForEach, virHashForEachSorted, virHashForEachSafe
 * @table: the hash table to process
 * @iter: callback to process each element
 * @opaque: opaque data to pass to the iterator
 *
 * Iterates over every element in the hash table, invoking the 'iter' callback.
 *
 * The elements are iterated in arbitrary order.
 *
 * virHashForEach prohibits @iter from modifying @table
 *
 * virHashForEachSafe allows the callback to remove the current
 * element using virHashRemoveEntry but calling other virHash* functions is
 * prohibited. Note that removing the entry invalidates @key and @payload in
 * the callback.
 *
 * virHashForEachSorted iterates the elements in order by sorted key.
 *
 * virHashForEachSorted and virHashForEachSafe are more computationally
 * expensive than virHashForEach.
 *
 * If @iter fails and returns a negative value, the evaluation is stopped and -1
 * is returned.
 *
 * Returns 0 on success or -1 on failure.
 */
int
virHashForEach(virHashTablePtr table, virHashIterator iter, void *opaque)
{
    size_t i;
    int ret = -1;

    if (table == NULL || iter == NULL)
        return -1;

    for (i = 0; i < table->size; i++) {
        virHashEntryPtr entry = table->table[i];
        while (entry) {
            virHashEntryPtr next = entry->next;
            ret = iter(entry->payload, entry->name, opaque);

            if (ret < 0)
                return ret;

            entry = next;
        }
    }

    return 0;
}


int
virHashForEachSafe(virHashTablePtr table,
                   virHashIterator iter,
                   void *opaque)
{
    g_autofree virHashKeyValuePairPtr items = virHashGetItems(table, NULL, false);
    size_t i;

    if (!items)
        return -1;

    for (i = 0; items[i].key; i++) {
        if (iter((void *)items[i].value, items[i].key, opaque) < 0)
            return -1;
    }

    return 0;
}


int
virHashForEachSorted(virHashTablePtr table,
                     virHashIterator iter,
                     void *opaque)
{
    g_autofree virHashKeyValuePairPtr items = virHashGetItems(table, NULL, true);
    size_t i;

    if (!items)
        return -1;

    for (i = 0; items[i].key; i++) {
        if (iter((void *)items[i].value, items[i].key, opaque) < 0)
            return -1;
    }

    return 0;
}


/**
 * virHashRemoveSet
 * @table: the hash table to process
 * @iter: callback to identify elements for removal
 * @opaque: opaque data to pass to the iterator
 *
 * Iterates over all elements in the hash table, invoking the 'iter'
 * callback. If the callback returns a non-zero value, the element
 * will be removed from the hash table & its payload passed to the
 * data freer callback registered at creation.
 *
 * Returns number of items removed on success, -1 on failure
 */
ssize_t
virHashRemoveSet(virHashTablePtr table,
                 virHashSearcher iter,
                 const void *opaque)
{
    size_t i, count = 0;

    if (table == NULL || iter == NULL)
        return -1;

    for (i = 0; i < table->size; i++) {
        virHashEntryPtr *nextptr = table->table + i;

        while (*nextptr) {
            virHashEntryPtr entry = *nextptr;
            if (!iter(entry->payload, entry->name, opaque)) {
                nextptr = &entry->next;
            } else {
                count++;
                if (table->dataFree)
                    table->dataFree(entry->payload);
                g_free(entry->name);
                *nextptr = entry->next;
                VIR_FREE(entry);
                table->nbElems--;
            }
        }
    }

    return count;
}

static int
_virHashRemoveAllIter(const void *payload G_GNUC_UNUSED,
                      const char *name G_GNUC_UNUSED,
                      const void *opaque G_GNUC_UNUSED)
{
    return 1;
}

/**
 * virHashRemoveAll
 * @table: the hash table to clear
 *
 * Free the hash @table's contents. The userdata is
 * deallocated with the function provided at creation time.
 */
void
virHashRemoveAll(virHashTablePtr table)
{
    virHashRemoveSet(table, _virHashRemoveAllIter, NULL);
}

/**
 * virHashSearch:
 * @table: the hash table to search
 * @iter: an iterator to identify the desired element
 * @opaque: extra opaque information passed to the iter
 * @name: the name of found user data, pass NULL to ignore
 *
 * Iterates over the hash table calling the 'iter' callback
 * for each element. The first element for which the iter
 * returns non-zero will be returned by this function.
 * The elements are processed in a undefined order. Caller is
 * responsible for freeing the @name.
 */
void *virHashSearch(virHashTablePtr table,
                    virHashSearcher iter,
                    const void *opaque,
                    char **name)
{
    size_t i;


    if (table == NULL || iter == NULL)
        return NULL;

    for (i = 0; i < table->size; i++) {
        virHashEntryPtr entry;
        for (entry = table->table[i]; entry; entry = entry->next) {
            if (iter(entry->payload, entry->name, opaque)) {
                if (name)
                    *name = g_strdup(entry->name);
                return entry->payload;
            }
        }
    }

    return NULL;
}


struct virHashGetItemsIteratorData {
    virHashKeyValuePair *items;
    size_t i;
};


static int
virHashGetItemsIterator(void *payload,
                        const char *key,
                        void *opaque)
{
    struct virHashGetItemsIteratorData *data = opaque;

    data->items[data->i].key = key;
    data->items[data->i].value = payload;

    data->i++;
    return 0;
}


static int
virHashGetItemsKeySorter(const void *va,
                         const void *vb)
{
    const virHashKeyValuePair *a = va;
    const virHashKeyValuePair *b = vb;

    return strcmp(a->key, b->key);
}


virHashKeyValuePairPtr
virHashGetItems(virHashTablePtr table,
                size_t *nitems,
                bool sortKeys)
{
    size_t dummy;
    struct virHashGetItemsIteratorData data = { .items = NULL, .i = 0 };

    if (!nitems)
        nitems = &dummy;

    *nitems = virHashSize(table);

    data.items = g_new0(virHashKeyValuePair, *nitems + 1);

    virHashForEach(table, virHashGetItemsIterator, &data);

    if (sortKeys)
        qsort(data.items, *nitems, sizeof(* data.items), virHashGetItemsKeySorter);

    return data.items;
}


struct virHashEqualData
{
    bool equal;
    virHashTablePtr table2;
    virHashValueComparator compar;
};

static int virHashEqualSearcher(const void *payload, const char *name,
                                const void *opaque)
{
    struct virHashEqualData *vhed = (void *)opaque;
    const void *value;

    value = virHashLookup(vhed->table2, name);
    if (!value ||
        vhed->compar(value, payload) != 0) {
        /* key is missing in 2nd table or values are different */
        vhed->equal = false;
        /* stop 'iteration' */
        return 1;
    }
    return 0;
}

bool virHashEqual(virHashTablePtr table1,
                  virHashTablePtr table2,
                  virHashValueComparator compar)
{
    struct virHashEqualData data = {
        .equal = true,
        .table2 = table2,
        .compar = compar,
    };

    if (table1 == table2)
        return true;

    if (!table1 || !table2 ||
        virHashSize(table1) != virHashSize(table2))
        return false;

    virHashSearch(table1, virHashEqualSearcher, &data, NULL);

    return data.equal;
}
