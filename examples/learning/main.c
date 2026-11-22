#include "wada_boot.h"
#include "wada_store_sqlite.h"

extern const wada_product learning_product;

int main(int argc, char **argv) {
    const wada_product *products[] = { &learning_product };
    wada_boot_desc desc = {
        "Wada — Learn",
        1040,
        720,
        false,
        wada_store_sqlite(),
        products,
        1,
        "learning",
        "Wada Learning",
    };
    return wada_boot_run(&desc, argc, argv);
}
