/* Wada — the shipped app: two products on one core. */
#include "wada_boot.h"
#include "wada_store_sqlite.h"

extern const wada_product notes_product;
extern const wada_product learning_product;

int main(int argc, char **argv) {
    const wada_product *products[] = { &notes_product, &learning_product };
    wada_boot_desc desc = {
        "Wada",
        1180,
        780,
        false,
        wada_store_sqlite(),
        products,
        2,
        "notes",
        NULL,
    };
    return wada_boot_run(&desc, argc, argv);
}
