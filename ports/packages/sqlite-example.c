/* rozOS — SQLite-এর ছোট উদাহরণ (rozpkg install sqlite-এর পরে)
 *   tcc /usr/share/doc/sqlite/example.c -lsqlite3 -o /tmp/s && /tmp/s
 * একটা ডেটাবেস ফাইল বানায়, সারি যোগ করে (prepared statement), তারপর খুঁজে পড়ে। */

#include <stdio.h>
#include <sqlite3.h>

int main(void)
{
    sqlite3 *db;
    sqlite3_stmt *st;
    const char *names[] = { "kernel", "libc", "tcc", "rozwm" };
    int i, rc;

    if (sqlite3_open("/tmp/example.db", &db) != SQLITE_OK) {
        printf("open: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    rc = sqlite3_exec(db, "DROP TABLE IF EXISTS part;"
                          "CREATE TABLE part(id INTEGER PRIMARY KEY, name TEXT, lines INTEGER);",
                      NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("create: %s\n", sqlite3_errmsg(db)); return 1; }

    sqlite3_prepare_v2(db, "INSERT INTO part(name, lines) VALUES(?, ?)", -1, &st, NULL);
    for (i = 0; i < 4; i++) {
        sqlite3_bind_text(st, 1, names[i], -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, (i + 1) * 1000);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "SELECT id, name, lines FROM part WHERE lines >= ? ORDER BY lines DESC", -1, &st, NULL);
    sqlite3_bind_int(st, 1, 2000);
    while (sqlite3_step(st) == SQLITE_ROW)
        printf("%d  %-6s %d\n", sqlite3_column_int(st, 0), (const char *)sqlite3_column_text(st, 1),
               sqlite3_column_int(st, 2));
    sqlite3_finalize(st);
    printf("SQLite %s\n", sqlite3_libversion());
    sqlite3_close(db);
    return 0;
}
