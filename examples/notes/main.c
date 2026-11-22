#include "wada_boot.h"
#include "wada_store_sqlite.h"

extern const wada_product notes_product;

int main(int argc, char **argv) {
    const wada_product *products[] = { &notes_product };
    wada_boot_desc desc = {
        "Wada — Notes",
        1180,
        780,
        false,
        wada_store_sqlite(),
        products,
        1,
        "notes",
        "Wada Notes",
    };
    return wada_boot_run(&desc, argc, argv);
}
