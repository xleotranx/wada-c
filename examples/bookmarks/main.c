#include "wada_boot.h"
#include "wada_store_sqlite.h"

extern const wada_product bookmarks_product;

int main(int argc, char **argv) {
    const wada_product *products[] = { &bookmarks_product };
    wada_boot_desc desc = {
        "Wada — Bookmarks",
        960,
        680,
        false,
        wada_store_sqlite(),
        products,
        1,
        "bookmarks",
        "Wada Bookmarks",
    };
    return wada_boot_run(&desc, argc, argv);
}
