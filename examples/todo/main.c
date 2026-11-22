#include "wada_boot.h"
#include "wada_store_sqlite.h"

extern const wada_product todo_product;

int main(int argc, char **argv) {
    const wada_product *products[] = { &todo_product };
    wada_boot_desc desc = {
        "Wada — Todo",
        900,
        700,
        false,
        wada_store_sqlite(),
        products,
        1,
        "todo",
        "Wada Todo",
    };
    return wada_boot_run(&desc, argc, argv);
}
