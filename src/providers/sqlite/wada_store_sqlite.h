/* SQLite implementation of the Wada store interface.
 *
 * This is the only file in the tree that includes sqlite3.h: the core and the
 * products see wada_store_ops only. Keys and values are byte slices; the
 * provider owns the database, the transaction and the iterator.
 */
#ifndef WADA_STORE_SQLITE_H
#define WADA_STORE_SQLITE_H

#include "wada_iface.h"

/* Returns the store vtable. The URL is a SQLite filename (":memory:" works). */
WADA_API const wada_store_ops *wada_store_sqlite(void);

#endif /* WADA_STORE_SQLITE_H */
